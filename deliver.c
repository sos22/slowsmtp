/* Delivery agent to go with slowsmtp. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "config.h"

#define MAX_LINE_LENGTH 4096

static const char *username;
static const char *password;

struct string_set {
	int n;
	int n_allocated;
	char **strings;
};

static void
string_set_init(struct string_set *x)
{
	bzero(x, sizeof(*x));
}

static void
string_set_push(struct string_set *x, char *str)
{
	if (x->n == x->n_allocated) {
		x->n_allocated += 4;
		x->strings = realloc(x->strings,
				     sizeof(x->strings[0]) *
				     x->n_allocated);
	}
	x->strings[x->n] = str;
	x->n++;
}

static void
string_set_cleanup(struct string_set *x)
{
	int i;
	for (i = 0; i < x->n; i++)
		free(x->strings[i]);
	free(x->strings);
	bzero(x, sizeof(*x));
}

static int
get_server_response(BIO *bio, char **args)
{
	char linebuf[MAX_LINE_LENGTH];
	int code;
	char *arg_start, *arg_end;

repeat:
	bzero(linebuf, sizeof(linebuf));
	if (BIO_gets(bio, linebuf, sizeof(linebuf) - 1) < 0)
		err(1, "BIO_gets(linebuf)");
	printf("Server said %s\n", linebuf);

	if (!isdigit(linebuf[0]) ||
	    !isdigit(linebuf[1]) ||
	    !isdigit(linebuf[2]) ||
	    (linebuf[3] != '-' &&
	     !(linebuf[3] == '\r' && linebuf[4] == '\n') &&
	     linebuf[3] != ' '))
		errx(1, "strange server response %s", linebuf);
	if (linebuf[3] == '-') {
		/* Continuation line */
		goto repeat;
	}

	code = (linebuf[0] - '0') * 100 +
		(linebuf[1] - '0') * 10 +
		(linebuf[2] - '0');
	if (args) {
		arg_start = linebuf + 3;
		while (isspace(arg_start[0]))
			arg_start++;
		arg_end = arg_start + strlen(arg_start) - 1;
		while (arg_end >= arg_start && isspace(arg_end[0]))
			arg_end--;
		*args = malloc(arg_end - arg_start + 2);
		memcpy(*args, arg_start, arg_end - arg_start + 1);
		(*args)[arg_end - arg_start + 1] = 0;
	}
	return code;
}

static void simple_command(BIO *b, const char *fmt, ...)
	__attribute__((format (printf, 2, 3)));
static void
simple_command(BIO *bio, const char *fmt, ...)
{
	va_list args;
	int code;

	va_start(args, fmt);
	BIO_vprintf(bio, fmt, args);
	va_end(args);
	(void)BIO_flush(bio);
	code = get_server_response(bio, NULL);
	if (code != 250)
		errx(1, "server rejected command %s", fmt);
}

static void
pump_data(const char *name, BIO *b)
{
	char *path;
	FILE *f;
	int r;
	char c;

	/* The sequence "\r\n." must be transmitted as "\r\n..".  The
	   magic_offset is 1 if we just saw '\r', 2 if we just saw
	   "\r\n", and 0 otherwise. */
	int magic_offset;

	asprintf(&path, "%s/spool/%s/body", SPOOL_DIR, name);
	f = fopen(path, "r");
	if (!f)
		err(1, "opening %s", path);
	magic_offset = 0;
	while (1) {
		r = fgetc_unlocked(f);
		if (r == EOF) {
			if (feof(f))
				break;
			err(1, "reading %s", path);
		}
		/* I really hope the BIO buffering is sensible... */
		c = r;
		BIO_write(b, &c, 1);

		/* Update escape encoding state.  By especially
		   careful of things like \r\r\r\n. or \r\n\r\n. */
		switch (magic_offset) {
		case 0:
			if (c == '\r')
				magic_offset = 1;
			break;
		case 1:
			if (c == '\n')
				magic_offset = 2;
			else if (c == '\r')
				magic_offset = 1;
			else
				magic_offset = 0;
			break;
		case 2:
			if (c == '.') {
				/* Transmit it twice */
				BIO_write(b, &c, 1);
			} else if (c == '\r') {
				magic_offset = 1;
			}
			magic_offset = 0;
			break;
		default:
			abort();
		}
	}

	/* All done.  Send the magic termination sequence. */
	BIO_write(b, "\r\n.\r\n", 5);
	(void)BIO_flush(b);

	fclose(f);
	free(path);
}

/* This is *very* specific to Hermes.  Oh well. */
static void
send_message(const char *name, const char *from, const struct string_set *to,
	     BIO *bio)
{
	int code;
	int i;

	code = get_server_response(bio, NULL);
	if (code != 220)
		errx(1, "server gave strange greeting code %d (expected 220)",
		     code);
	simple_command(bio, "EHLO localhost\r\n");

	BIO_printf(bio, "AUTH LOGIN\r\n");
	(void)BIO_flush(bio);
	code = get_server_response(bio, NULL);
	if (code != 334)
		errx(1,
		     "server rejected AUTH LOGIN, code %d when we wanted 334",
		     code);
	BIO_printf(bio, "%s\r\n", username);
	(void)BIO_flush(bio);
	code = get_server_response(bio, NULL);
	if (code != 334)
		errx(1, "server rejected username");
	BIO_printf(bio, "%s\r\n", password);
	(void)BIO_flush(bio);
	code = get_server_response(bio, NULL);
	if (code != 235)
		errx(1,"server rejected login");

	simple_command(bio, "MAIL FROM:<%s>\r\n", from);
	for (i = 0; i < to->n; i++)
		simple_command(bio, "RCPT TO:<%s>\r\n", to->strings[i]);

	BIO_printf(bio, "DATA\r\n");
	(void)BIO_flush(bio);
	code = get_server_response(bio, NULL);
	if (code != 354)
		errx(1, "server rejected DATA command");

	pump_data(name, bio);

	code = get_server_response(bio, NULL);
	if (code != 250)
		errx(1, "server flagged error after payload sent");

	BIO_printf(bio, "QUIT\r\n");
	(void)BIO_flush(bio);
	code = get_server_response(bio, NULL);
	if (code != 221)
		warnx("server rejected QUIT command?");

	/* Okay, it should now be away.  Go and remove the file from
	 * the spool. */
}

static void
forward(const char *name, const char *from, const struct string_set *to)
{
	int i;
	BIO *sbio;
	SSL_CTX *ctx;
	SSL *ssl;

	printf("Forward %s from %s, to ", name, from);
	for (i = 0; i < to->n; i++)
		printf("%s%s", to->strings[i],
		       i == to->n - 1 ? "\n" : ", ");

	ctx = SSL_CTX_new(SSLv23_client_method());

	/* XXX check certificate */
	/* XXX generally figure out what all of this stuff is
	 * doing. */

	sbio = BIO_new_ssl_connect(ctx);
	BIO_get_ssl(sbio, &ssl);
	if(!ssl)
		err(1, "Can't locate SSL pointer\n");
	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

	BIO_set_conn_port(sbio, "smtps");
	BIO_set_conn_hostname(sbio, UPSTREAM_SERVER);

	if(BIO_do_connect(sbio) <= 0) {
		fprintf(stderr, "Error connecting to server\n");
		ERR_print_errors_fp(stderr);
		exit(1);
	}

	if(BIO_do_handshake(sbio) <= 0) {
		fprintf(stderr, "Error establishing SSL connection\n");
		ERR_print_errors_fp(stderr);
		exit(1);
	}

	/* Turn on buffering */
	sbio = BIO_push(BIO_new(BIO_f_buffer()), sbio);

	send_message(name, from, to, sbio);

	BIO_free_all(sbio);
}

static long
consider_forwarding(const char *name)
{
	char *meta;
	FILE *meta_f;
	int r;
	time_t now;
	time_t arrival_time;
	long remaining;
	char *from_address;
	struct string_set to_addresses;

	asprintf(&meta, "%s/spool/%s/meta", SPOOL_DIR, name);
	meta_f = fopen(meta, "r");
	if (!meta_f)
		err(1, "fopen(%s)", meta);
	r = fscanf(meta_f, "w: %ld\n", &arrival_time);
	if (r < 0)
		err(1, "reading %s", meta);
	if (r != 1)
		errx(1, "%s appears to be corrupt?", meta);
	now = time(NULL);
	remaining = arrival_time + LATENCY - now;
	if (remaining > 0) {
		printf("%s not ready yet; %ld left.\n",
		       meta,
		       remaining);
		fclose(meta_f);
		free(meta);
		return remaining;
	}

	printf("%s is ready to send\n", meta);

	/* Ready to send, so finish slurping in the metadata */
	r = fscanf(meta_f, "f: %as\n", &from_address);
	if (r < 0)
		err(1, "reading from address in %s", meta);
	if (r != 1)
		errx(1, "%s contains no from address?", meta);
	string_set_init(&to_addresses);
	while (1) {
		char *to_address;
		r = fscanf(meta_f, "t: %as\n", &to_address);
		if (r <= 0) {
			if (feof(meta_f))
				break;
			if (r == 0)
				errno = EINVAL;
			err(1, "reading from addresses in %s",
			    meta);
		}
		string_set_push(&to_addresses, to_address);
	}
	fclose(meta_f);
	free(meta);

	forward(name, from_address, &to_addresses);

	string_set_cleanup(&to_addresses);
	free(from_address);

	return -1;
}

static char
base64_lookup(unsigned x)
{
	if (x <= 25)
		return 'A' + x;
	else if (x <= 51)
		return 'a' + x - 26;
	else if (x <= 61)
		return '0' + x - 52;
	else if (x == 62)
		return '+';
	else if (x == 63)
		return '/';
	else
		abort();
}

static char *
base64_encode(const char *src)
{
	int len = ((strlen(src) + 2) / 3) * 4;
	char *output = calloc(len + 1, 1);
	char *output_cursor = output;
	unsigned char inp_buffer[3];
	unsigned char out_buffer[4];
	int x;
	bool done = false;

	while (!done) {
		if (src[0] == 0)
			break;
		bzero(inp_buffer, sizeof(inp_buffer));
		inp_buffer[0] = src[0];
		inp_buffer[1] = src[1];
		if (src[1] != 0)
			inp_buffer[2] = src[2];
		out_buffer[0] = inp_buffer[0] >> 2;
		out_buffer[1] = (inp_buffer[0] << 4) | (inp_buffer[1] >> 4);
		out_buffer[2] = (inp_buffer[1] << 2) | (inp_buffer[2] >> 6);
		out_buffer[3] = inp_buffer[2];
		for (x = 0; x < 4; x++)
			out_buffer[x] = base64_lookup(out_buffer[x] % 64);

		if (src[1] == 0) {
			out_buffer[2] = '=';
			out_buffer[3] = '=';
			done = true;
		} else if (src[2] == 0) {
			out_buffer[3] = '=';
			done = true;
		}

		memcpy(output_cursor, out_buffer, 4);
		output_cursor += 4;
		src += 3;
	}
	return output;
}

int
main(int argc, char *argv[])
{
	DIR *d;
	struct dirent *de;
	long delay;
	char *p;

	SSL_library_init();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();

	username = base64_encode(getenv("SLOW_SMTP_USERNAME"));
	password = base64_encode(getenv("SLOW_SMTP_PASSWORD"));

	asprintf(&p, "%s/spool", SPOOL_DIR);

	while (1) {
		delay = POLL_TIME;
		d = opendir(p);
		if (!d)
			err(1, "opendir(%s)", p);
		while (1) {
			errno = 0;
			de = readdir(d);
			if (!de) {
				if (errno)
					err(1, "readdir(%s)", SPOOL_DIR);
				break;
			}
			if (strcmp(de->d_name, ".") &&
			    strcmp(de->d_name, "..")) {
				long dl = consider_forwarding(de->d_name);
				if (dl >= 0 && dl < delay)
					delay = dl;
			}
		}
		closedir(d);
		sleep(delay);
	}
}
