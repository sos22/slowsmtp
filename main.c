#define _GNU_SOURCE
#include <sys/fcntl.h>
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <ftw.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

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
/* Maximum number of recipients per message */
#define MAX_RECIPIENTS_PER_MESSAGE 512

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

	/* The mail reception state machine.  We fill out from_address
	   when we get the MAIL FROM command, then the rcpt_address
	   fields as we get the RCPT TO commands. */
	char *from_address;
	int nr_recips_allocated;
	int nr_recipients;
	char **recipients;

	/* The data spool is allocated when we receive the DATA
	 * command. */
	FILE *data_spool;
	char *data_spool_fname;
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

#define COMMANDS(x) \
	x(HELO)	    \
	x(QUIT)	    \
	x(EHLO)	    \
	x(MAIL)	    \
	x(RCPT)	    \
	x(DATA)	    \
	x(NOOP)

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

static void
_vqueue_response(bool continued,
		 int code, struct client *c,
		 const char *fmt, va_list args)
{
	char *msg;
	char *msg2;
	vasprintf(&msg, fmt, args);
	asprintf(&msg2, "%03d%c%s\r\n", code, continued ? '-' : ' ', msg);
	free(msg);
	queue_raw_string(c, msg2);
	free(msg2);
}

static void queue_response(int code, struct client *c,
			   const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void
queue_response(int code, struct client *c, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	_vqueue_response(false, code, c, fmt, args);
	va_end(args);
}

static void queue_response_continued(int code, struct client *c,
				     const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void
queue_response_continued(int code, struct client *c, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	_vqueue_response(true, code, c, fmt, args);
	va_end(args);
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
alloc_spool_file(char **fname, FILE **f)
{
	int cntr;
	char *path;
	int fd;

	*fname = NULL;
	*f = NULL;

	cntr = 0;
	while (1) {
		asprintf(&path, "%s/tmp/%d",
			 SPOOL_DIR, cntr);
		fd = open(path, O_WRONLY|O_CREAT|O_EXCL, 0600);
		if (fd >= 0)
			break;
		if (errno != EEXIST) {
			free(path);
			return false;
		}
		cntr++;
	}

	*f = fdopen(fd, "w");
	if (!*f) {
		close(fd);
		free(path);
		return false;
	}
	*fname = path;
	return true;
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

static bool
handle_EHLO(struct client *c, char *parameters)
{
	printf("EHLO from %s.\n", parameters);
	queue_response(250, c, "Hello %.128s", parameters);
	return true;
}

static char *
skip_whitespace(const char *x)
{
	while (isspace(x[0]))
		x++;
	return (char *)x;
}

static char *
parse_bracketed_address(const char *input)
{
	int len = strlen(input);
	char *b;

	if (len == 0 || input[0] != '<' || input[len-1] != '>')
		return NULL;
	b = malloc(len - 1);
	memcpy(b, input + 1, len - 2);
	b[len-2] = 0;
	/* We use line-delimited files later.  Make sure we don't
	 * confuse ourselves. */
	/* XXX I'm tempted to require that everything pass isprint()
	   here, just for sanity, but it's not strictly needed and it
	   might cause problems for UTF-8 email addresses etc. */
	if (strchr(b, '\n')) {
	    free(b);
	    return NULL;
	}
	return b;
}

static bool
handle_MAIL(struct client *c, char *parameters)
{
	if (c->from_address) {
		queue_response(503, c, "Already have a from address");
		return true;
	}
	if (strncasecmp(parameters, "from:", 5)) {
		queue_response(501, c, "expected from:, got %.128s",
			       parameters);
		return true;
	}
	c->from_address = parse_bracketed_address(skip_whitespace(parameters+5));
	if (!c->from_address) {
		queue_response(501, c, "from: address in MAIL command must be enclosed in <>");
	} else {
		printf("Have a from address %s.\n", c->from_address);
		queue_response(250, c, "OK");
	}
	return true;
}

static bool
handle_RCPT(struct client *c, char *parameters)
{
	char *addr;

	if (!c->from_address) {
		queue_response(503, c, "Need MAIL FROM first");
		return true;
	}
	if (c->nr_recipients == MAX_RECIPIENTS_PER_MESSAGE) {
		queue_response(552, c, "too many recipients");
		return true;
	}
	if (strncasecmp(parameters, "to:", 3)) {
		queue_response(501, c, "expected to:, got %.128s",
			       parameters);
		return true;
	}
	addr = parse_bracketed_address(skip_whitespace(parameters+3));
	if (!addr) {
		queue_response(501, c, "failed to parse %.128s",
			       parameters);
		return true;
	}

	if (c->nr_recipients == c->nr_recips_allocated) {
		c->nr_recips_allocated += 4;
		c->recipients = realloc(c->recipients,
					sizeof(c->recipients[0]) *
					c->nr_recips_allocated);
	}
	c->recipients[c->nr_recipients] = addr;
	c->nr_recipients++;
	queue_response(250, c, "receipt to %.128s", addr);
	return true;
}

static bool
handle_NOOP(struct client *c, char *parameters)
{
	queue_response(250, c, "NOOP");
	return true;
}

static bool
handle_DATA(struct client *c, char *parameters)
{
	if (strcmp(parameters, "")) {
		queue_response(501, c, "DATA takes no parameters");
		return true;
	}
	if (!c->from_address || c->nr_recipients == 0) {
		queue_response(503, c, "Need a sender and at least one recipient first");
		return true;
	}
	if (c->data_spool_fname || c->data_spool) {
		/* Not quite sure how this can happen... */
		queue_response(503, c, "sequence error");
		return true;
	}

	if (!alloc_spool_file(&c->data_spool_fname, &c->data_spool)) {
		/* It's not immensely clear whether we should clear
		   MAIL FROM: and RCPT TO: state at this point.  We
		   choose not to, but it doesn't actually matter a
		   great deal. */
		free(c->data_spool_fname);
		c->data_spool_fname = NULL;
		queue_response(552, c, "failed to allocate spool area");
		return true;
	}

	queue_response(354, c, "Go ahead");
	return true;
}

/* Drop all of the per-message state, including on the filesystem. */
static void
flush_state(struct client *c)
{
	int i;

	if (c->data_spool) {
		fclose(c->data_spool);
		c->data_spool = NULL;
	}
	if (c->data_spool_fname) {
		unlink(c->data_spool_fname);
		free(c->data_spool_fname);
		c->data_spool_fname = NULL;
	}
	free(c->from_address);
	c->from_address = NULL;
	for (i = 0; i < c->nr_recipients; i++)
		free(c->recipients[i]);
	free(c->recipients);
	c->recipients = NULL;
	c->nr_recipients = 0;
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
	flush_state(c);
	free(c);
}

static bool
process_command(struct client *c,
		char *command)
{
	int i;
	union smtp_verb verb;
	char *parameters;

	printf("Received command %s\n", command);
	/* Validate and receive verb */
	for (i = 0; i < 4; i++) {
		if (!isprint(command[i])) { /* Covers the NUL byte case */
			client_error(c);
			return false;
		}
		verb.name[i] = toupper(command[i]);
	}
	parameters = command + 4;
	while (isspace(parameters[0]))
		parameters++;
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (verb.key == commands[i].k.key)
			return commands[i].f(c, parameters);
	}
	queue_response(500, c, "verb %.4s not recognized",
		       verb.name);
	return true;
}

static bool
allocate_delivery_slot(char **final_slot, char **temporary_slot)
{
	int cntr;
	char *final_path;
	char *temporary_path;
	struct stat st;
	int i;

	/* Create a fresh directory which we're going to do delivery
	   into, and also lock the final slot so that nobody else can
	   create it.  The clever/hacky bit is that the temporary
	   directory is the lock on the final one.  The simplest
	   algorithm for doing so look slike this:

	   while (1) {
	      (temp, final) = invent_names();
	      if (mkdir(temp) < 0 && errno == EEXIST)
	          continue; // Try again
	      if (stat(final) != ENOENT) {
                  rmdir(temp);
                  continue;
              }
              return;
	   }

	   The only way of creating the final is to create the
	   temporary and rename it into place.  The only way of
	   creating a temporary is via this function, and there's a
	   one-to-one mapping between temps and finals.  You can't
	   create a temporary once we've gotten past the mkdir(), and
	   if we get past the stat then we know that no final exists
	   and no temporary does either.

	   (This assumes that rename is atomic, which it is on all
	   sensible filesystems.)

	   We optimise slightly by stat'ing the final before doing the
	   mkdir, which avoids a bunch of redundant mkdir()s when
	   there are already lots of queued messages, but that's a
	   nice easy optimisation.
	*/

	cntr = 0;
	final_path = temporary_path = NULL;
	while (1) {
		cntr++;
		free(final_path);
		free(temporary_path);
		asprintf(&final_path, "%s/spool/%d",
			 SPOOL_DIR, cntr);
		asprintf(&temporary_path, "%s/incoming/%d",
			 SPOOL_DIR, cntr);
		i = stat(final_path, &st);
		if (i >= 0)
			continue;
		if (errno != ENOENT)
			goto failed;

		i = mkdir(temporary_path, 0700);
		if (i < 0) {
			if (errno == EEXIST)
				continue;
			goto failed;
		}

		i = stat(final_path, &st);
		if (i < 0 && errno == ENOENT) {
			/* We're done */
			*final_slot = final_path;
			*temporary_slot = temporary_path;
			return true;
		}
		rmdir(temporary_path);
		if (i < 0)
			goto failed;
	}

failed:
	/* Some unexpected error in either stat() or mkdir() */
	free(final_path);
	free(temporary_path);
	return false;
}

static int
_recursive_remove_ftw(const char *fpath, const struct stat *st,
		      int typeflag, struct FTW *ftw)
{
	printf("Kill %s\n", fpath);

	/* Call me chicken... */
#if 0
	if (typeflag == FTW_F)
		unlink(fpath);
	else if (typeflag == FTW_D)
		rmdir(fpath);
#endif

	return 0;
}

static void
recursive_remove(const char *base)
{
	nftw(base, _recursive_remove_ftw, 50, FTW_DEPTH|FTW_PHYS);
}


static void
finished_message(struct client *c)
{
	char *slot;
	char *tslot;
	char *path;
	int i;
	FILE *f;
	bool success;

	success = false;

	path = slot = tslot = NULL;
	f = NULL;

	i = fclose(c->data_spool);
	c->data_spool = NULL;
	if (i == EOF) {
		/* Well, that was a waste of effort */
		goto err;
	}

	if (!allocate_delivery_slot(&slot, &tslot))
		goto err;

	asprintf(&path, "%s/body", tslot);
	if (rename(c->data_spool_fname, path) < 0)
		goto err;
	free(path);
	path = NULL;
	free(c->data_spool_fname);
	c->data_spool_fname = NULL;

	asprintf(&path, "%s/meta", tslot);
	f = fopen(path, "w");
	free(path);
	path = NULL;
	if (!f)
		goto err;
	fprintf(f, "w: %ld\n", time(NULL));
	fprintf(f, "f: %s\n", c->from_address);
	for (i = 0; i < c->nr_recipients; i++)
		fprintf(f, "t: %s\n", c->recipients[i]);
	i = fclose(f);
	f = NULL;
	if (i == EOF)
		goto err;

	if (rename(tslot, slot) < 0)
		goto err;

	/* We're done */
	success = true;

err:
	if (f)
	    fclose(f);
	if (tslot && !success)
		recursive_remove(tslot);
	free(tslot);
	free(slot);
	free(path);
	flush_state(c);
	if (success) {
		/* This is aguably a bit of an abuse (should really
		   sync() before sending this response), but it's good
		   enough for the intended purposes. */
		queue_response(250, c, "OK");
	} else {
		queue_response(552, c, "error in spooling");
	}
}

static void
receive_data(struct client *c)
{
	int idx;

	for (idx = c->rx_buf_cons;
	     idx < c->rx_buf_prod - 4;
		) {
		if (!memcmp(c->rx_buf + idx,
			    "\r\n.\r\n",
			    5)) {
			/* We're done */
			c->rx_buf_cons = idx + 5;
			finished_message(c);
			return;
		}
		if (!memcmp(c->rx_buf + idx,
			    "\r\n.",
			    3)) {
			/* SMTP period escape */
			fputc_unlocked('\r', c->data_spool);
			fputc_unlocked('\n', c->data_spool);
			idx += 3;
		} else {
			fputc_unlocked(c->rx_buf[idx], c->data_spool);
			idx++;
		}
	}

	/* Wait to receive more data */
	c->rx_buf_cons = idx;
	return;
}

static void
run_rx_command_machine(struct client *c)
{
	int command_end;
	int command_start;

	/* DATA commands are special */
	if (c->data_spool) {
		receive_data(c);
		return;
	}

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
