/* How long to delay messages for, in seconds */
#define LATENCY (15 * 60)

/* How often the delivery agent should poll for new messages, in
   seconds. */
#define POLL_TIME (15 * 60)


/* Where to stash messages while we're waiting to delivery them. */
#define SPOOL_DIR "spool"

/* Where to send stuff too when we do send it. */
#define UPSTREAM_SERVER "smtp.hermes.cam.ac.uk"
