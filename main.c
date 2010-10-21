#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Try to avoid RX'ing less than this in one receive, to try to reduce
   the risk of silly window syndrome. */
#define MIN_RX_SIZE 4096
/* Limit each client to consume no more than 1MB of memory */
#define MAX_BUFFER_SIZE (1 << 20)
/* Expand RX buffer by a couple of pages at a time */
#define RX_BUF_INCREMENT (4096 * 4)
/* Sanity check: send no individual response bigger than this */
#define MAX_TX_MESSAGE_SIZE (4096 * 4)
/* TX buffer size is always a multiple of this.  Must be a power of
   two. */
#define TX_BUF_INCREMENT 1024

struct listen_socket {
	int fd;
};

struct waitblock {
	int nr_polls_allocated;
	int nr_polls;
	struct pollfd *polls;
};

struct client {
	struct clientpool *clientpool;
	struct client *prev, *next;
	int fd;

	char *rx_buf;
	int rx_buf_cons;
	int rx_buf_prod;
	int rx_buf_size;

	char *tx_buf;
	int tx_buf_cons;
	int tx_buf_prod;
	int tx_buf_size;

	bool quit_received;
};

struct clientpool {
	struct client *head_client;
	struct waitblock *w;
};

union smtp_verb {
	unsigned char name[4];
	unsigned key;
};

struct command_handler {
	union smtp_verb k;
	bool (*f)(struct client *c, char *parameters);
};

#define COMMANDS(x) x(HELO) x(QUIT)

#define _mk_proto(x) \
static bool handle_ ## x (struct client *c, char *parameters);
COMMANDS(_mk_proto)
#undef _mk_proto

static const struct command_handler
commands[] = {
#define _mk_handler(x) { { #x }, handle_ ## x },
	COMMANDS(_mk_handler)
#undef _mk_handler
};

static long
my_strtol(const char *x, int *err)
{
	char *end;
	long r;

	*err = 0;
	errno = 0;
	r = strtol(x, &end, 10);
	if (!end || *end != 0) {
		errno = EINVAL;
		*err = 1;
		return 0;
	}
	if (errno != 0) {
		*err = 1;
		return 0;
	}
	return r;
}

static int
port_for_service(const char *name)
{
	int err;
	long port;
	struct servent *se;

	se = getservbyname(name, "tcp");
	endservent();
	if (se)
		return se->s_port;
	port = my_strtol(name, &err);
	if (err)
		errx(1,
		     "%s isn't a service name or a simple number",
		     name);
	if (port < 0 || port > 65535)
		errx(1, "%ld is out of range for a port number", port);
	return htons(port);
}

static struct listen_socket *
listen_on(const char *port)
{
	struct sockaddr_in sin;
	int fd;
	struct listen_socket *ls;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = port_for_service(port);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		err(1, "socket() for listen");
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		err(1, "bind() to %d", ntohs(sin.sin_port));
	if (listen(fd, 5) < 0)
		err(1, "listen() on %d", ntohs(sin.sin_port));
	ls = calloc(sizeof(*ls), 1);
	ls->fd = fd;
	return ls;
}

static struct waitblock *
new_waitblock()
{
	return calloc(sizeof(struct waitblock), 1);
}

static void
subscribe(struct waitblock *w, int fd, int mode)
{
	int x;

	for (x = 0; x < w->nr_polls; x++) {
		if (w->polls[x].fd == fd) {
			w->polls[x].events |= mode;
			return;
		}
	}
	if (w->nr_polls == w->nr_polls_allocated) {
		w->nr_polls_allocated += 4;
		w->polls = realloc(w->polls,
				   sizeof(struct pollfd) * w->nr_polls_allocated);
	}
	w->polls[w->nr_polls].fd = fd;
	w->polls[w->nr_polls].events = mode;
	w->polls[w->nr_polls].revents = 0;
	w->nr_polls++;
}

static void
unsubscribe(struct waitblock *w, int fd, int mode)
{
	int x;
	for (x = 0; x < w->nr_polls; x++) {
		assert(w->polls[x].events != 0);
		if (w->polls[x].fd == fd) {
			w->polls[x].events &= ~mode;
			if (!w->polls[x].events) {
				memmove(w->polls + x,
					w->polls + x + 1,
					sizeof(w->polls[x]) * (w->nr_polls - x - 1));
				w->nr_polls--;
			}
		}
	}
}

static int
waiton(struct waitblock *w, int *mode)
{
	int x;
	short *m;

	while (1) {
		for (x = 0; x < w->nr_polls; x++) {
			if (w->polls[x].revents) {
				m = &w->polls[x].revents;
				*mode = 0;
				if (*m & POLLPRI)
					*mode = POLLPRI;
				else if (*m & POLLRDHUP)
					*mode = POLLRDHUP;
				else if (*m & POLLERR)
					*mode = POLLERR;
				else if (*m & POLLHUP)
					*mode = POLLHUP;
				else if (*m & POLLNVAL)
					*mode = POLLNVAL;
				else if (*m & POLLIN)
					*mode = POLLIN;
				else if (*m & POLLOUT)
					*mode = POLLOUT;
				if (*mode == 0)
					errx(1, "unknown poll bit %d set",
					     *m);
				*m &= ~*mode;
				return w->polls[x].fd;
			}
		}
		if (poll(w->polls, w->nr_polls, -1) < 0)
			err(1, "poll(%d)", w->nr_polls);
	}
}

static struct clientpool *
new_clientpool(struct waitblock *w)
{
	struct clientpool *c = calloc(sizeof(*c), 1);
	c->w = w;
	return c;
}

static void
queue_raw_string(struct client *c, char *msg)
{
	int len = strlen(msg);
	assert(len < MAX_TX_MESSAGE_SIZE);
	if (len + c->tx_buf_prod > c->tx_buf_size) {
		memmove(c->tx_buf,
			c->tx_buf + c->tx_buf_cons,
			c->tx_buf_prod - c->tx_buf_cons);
		c->tx_buf_prod -= c->tx_buf_cons;
		c->tx_buf_cons = 0;
	}
	if (len + c->tx_buf_prod > c->tx_buf_size) {
		c->tx_buf_size =
			(len + c->tx_buf_prod + TX_BUF_INCREMENT - 1) &
			~(TX_BUF_INCREMENT - 1);
		c->tx_buf = realloc(c->tx_buf, c->tx_buf_size);
	}
	memcpy(c->tx_buf + c->tx_buf_prod, msg, len);
	c->tx_buf_prod += len;
	subscribe(c->clientpool->w,
		  c->fd,
		  POLLOUT);
}

static void queue_response(int code, struct client *c,
			   const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void
queue_response(int code, struct client *c, const char *fmt, ...)
{
	va_list args;
	char *msg;
	char *msg2;

	va_start(args, fmt);
	vasprintf(&msg, fmt, args);
	va_end(args);
	asprintf(&msg2, "%03d %s\r\n", code, msg);
	free(msg);
	queue_raw_string(c, msg2);
	free(msg2);
}

static void
new_client(struct listen_socket *l, struct clientpool *cp)
{
	int newfd;
	struct client *c;

	newfd = accept(l->fd, NULL, NULL);
	if (newfd < 0)
		err(1, "accepting new client");
	c = calloc(sizeof(*c), 1);
	c->fd = newfd;
	c->clientpool = cp;
	c->prev = NULL;
	c->next = cp->head_client;
	if (cp->head_client)
		cp->head_client->prev = c;
	cp->head_client = c;
	queue_response(220, c, "slowsmtp ready");
	subscribe(cp->w, c->fd, POLLIN);
}

static void
server_socket_did_something(struct listen_socket *l,
			    struct clientpool *cp,
			    int mode)
{
	switch (mode) {
	case POLLIN:
		new_client(l, cp);
		break;
	case POLLERR:
		errx(1, "error on main listening socket");
	default:
		errx(1, "unexpected poll status %d on main listening socket",
		     mode);
	}
}

static bool
handle_HELO(struct client *c, char *parameters)
{
	printf("Connection from %s.\n", parameters);
	queue_response(250, c, "Hello %.128s", parameters);
	return true;
}

static bool
handle_QUIT(struct client *c, char *parameters)
{
	queue_response(221, c, "Goodbye");
	c->quit_received = true;
	return true;
}

static void
client_error(struct client *c)
{
	/* The client has experience an error and needs to go away
	 * now. */
	close(c->fd);
	if (c->prev)
		c->prev->next = c->next;
	else
		c->clientpool->head_client = c->next;
	if (c->next)
		c->next->prev = c->prev;
	unsubscribe(c->clientpool->w, c->fd, POLLIN|POLLOUT);
	free(c->rx_buf);
	free(c->tx_buf);
	free(c);
}

static bool
process_command(struct client *c,
		char *command)
{
	int i;
	union smtp_verb verb;
	char *parameters;

	/* Validate and receive verb */
	for (i = 0; i < 4; i++) {
		if (!isprint(command[i])) { /* Covers the NUL byte case */
			client_error(c);
			return false;
		}
		verb.name[i] = toupper(command[i]);
	}
	if (command[4] == 0)
		parameters = command + 4;
	else
		parameters = command + 5;
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (verb.key == commands[i].k.key)
			return commands[i].f(c, parameters);
	}
	queue_response(500, c, "verb %.4s not recognized",
		       verb.name);
	return true;
}

static void
run_rx_command_machine(struct client *c)
{
	int command_end;
	int command_start;

	/* Parse up the next command.  Just goes to the end of a
	 * line. */
	command_start = c->rx_buf_cons;
	for (command_end = command_start;
	     command_end + 1 < c->rx_buf_prod &&
		     c->rx_buf[command_end] != '\r' &&
		     c->rx_buf[command_end+1] != '\n';
	     command_end++)
		;
	if (command_end + 1 == c->rx_buf_prod) {
		printf("No command.\n");
		return;
	}
	c->rx_buf[command_end] = 0;
	if (process_command(c, c->rx_buf + command_start))
		c->rx_buf_cons = command_end + 2;
}

static void
client_can_read(struct client *c)
{
	ssize_t r;

#warning Do something else for DATA command bodies

	if (c->rx_buf_size - c->rx_buf_prod < MIN_RX_SIZE) {
		memmove(c->rx_buf, c->rx_buf + c->rx_buf_cons,
			c->rx_buf_prod - c->rx_buf_cons);
		c->rx_buf_prod -= c->rx_buf_cons;
		c->rx_buf_cons = 0;
	}
	if (c->rx_buf_size == c->rx_buf_prod) {
		/* The client sent a single command which is too big
		   for our buffer.  Make a bigger buffer, up to some
		   limit. */
		if (c->rx_buf_size >= MAX_BUFFER_SIZE) {
			/* Uh oh.  Assume the client is doing
			   something nasty and kick them out. */
			warnx("client sent an enormous command (currently %d and growing)",
			      c->rx_buf_size);
			client_error(c);
			return;
		}
		c->rx_buf_size += RX_BUF_INCREMENT;
		if (c->rx_buf_size > MAX_BUFFER_SIZE)
			c->rx_buf_size = MAX_BUFFER_SIZE;
		c->rx_buf = realloc(c->rx_buf, c->rx_buf_size);
	}
	r = read(c->fd, c->rx_buf + c->rx_buf_prod,
		 c->rx_buf_size - c->rx_buf_prod);
	if (r < 0) {
		client_error(c);
		return;
	}
	if (r == 0) {
		/* Not really an error, but treat it as one anyway. */
		client_error(c);
		return;
	}
	c->rx_buf_prod += r;

	run_rx_command_machine(c);
}

static void
client_can_write(struct client *c)
{
	int s;

	s = write(c->fd, c->tx_buf + c->tx_buf_cons,
		  c->tx_buf_prod - c->tx_buf_cons);
	if (s <= 0) {
		client_error(c);
		return;
	}
	c->tx_buf_cons += s;

	if (c->tx_buf_cons == c->tx_buf_prod) {
		if (c->quit_received) {
			/* Not really an error, but it amounts to the same
			   thing. */
			client_error(c);
			return;
		}
		unsubscribe(c->clientpool->w,
			    c->fd,
			    POLLOUT);
	}
}

static void
client_did_something(int fd, struct clientpool *cp, int mode)
{
	struct client *c;
	for (c = cp->head_client; c && c->fd != fd; c = c->next)
		;
	assert(c);
	switch (mode) {
	case POLLIN:
		client_can_read(c);
		return;
	case POLLOUT:
		client_can_write(c);
		return;
	case POLLERR:
	case POLLHUP:
		client_error(c);
		return;
	default:
		errx(1, "unexpected poll event %d from client", mode);
	}
}

int
main(int argc, char *argv[])
{
	struct listen_socket *l;
	struct waitblock *w;
	struct clientpool *cp;
	int fd;
	int mode;

	l = listen_on(argv[1]);
	if (!l)
		err(1, "listening on %s port", argv[1]);
	w = new_waitblock();
	subscribe(w, l->fd, POLLIN);

	cp = new_clientpool(w);

	while (1) {
		fd = waiton(w, &mode);
		if (fd < 0)
			err(1, "waiting");
		if (fd == l->fd)
			server_socket_did_something(l, cp, mode);
		else
			client_did_something(fd, cp, mode);
	}
}
