#define _XOPEN_SOURCE 700

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define FLIGHTS_FILE "flights.csv"
#define RESERVATIONS_FILE "reservations.csv"

#define MAX_FLIGHTS 256
#define MAX_SEATS 256

#define CITY_LEN 64
#define DATE_LEN 32
#define PASSPORT_LEN 32
#define COUNTRY_LEN 32
#define NAME_LEN 128

#define MIN_CONNECTION_MINUTES 45
#define MAX_CONNECTION_HOURS 24

typedef struct {
    char passport[PASSPORT_LEN];
    char country[COUNTRY_LEN];
    char full_name[NAME_LEN];
} Passenger;

typedef struct {
    int occupied;
    int reservation_id;
    Passenger passenger;
} Seat;

typedef struct {
    int id;

    char from[CITY_LEN];
    char to[CITY_LEN];

    char depart_str[DATE_LEN];
    char arrive_str[DATE_LEN];

    time_t depart_ts;
    time_t arrive_ts;

    int total_seats;

    /*
     * O pinakas einai 1-based.
     * Dhladh xrhsimopoioume thesh 1 mexri total_seats.
     */
    Seat seats[MAX_SEATS + 1];

    /*
     * Mutex ana pthsh.
     * Auto einai san record-level locking.
     * Den kleidwnoume olo to systhma, mono th sygkekrimenh pthsh.
     */
    pthread_mutex_t lock;

} Flight;

static Flight flights[MAX_FLIGHTS];
static int flight_count = 0;

/*
 * Koinh metavlhth gia ola ta threads.
 * Gia auto prepei na prostateuetai me mutex.
 */
static int next_reservation_id = 1;

/*
 * Auto to mutex prostateuei to arxeio reservations.csv
 * kai th metavlhth next_reservation_id.
 */
static pthread_mutex_t reservation_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void trim_newline(char *s) {
    if (s == NULL) {
        return;
    }

    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void trim_spaces(char *s) {
    if (s == NULL) {
        return;
    }

    char *start = s;

    while (*start == ' ' || *start == '\t') {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        n--;
    }
}

/*
 * Den epitrepoume ton xarakthra |
 * giati ton xrhsimopoioume ws diaxwristiko.
 */
static int has_forbidden_delimiter(const char *s) {
    if (s == NULL) {
        return 0;
    }

    return strchr(s, '|') != NULL;
}

/*
 * To passport prepei na exei to poly 9 xarakthres,
 * mono kefalaia grammata kai arithmous (xwris kena
 * h eidikous xarakthres opws to "<").
 */
static int is_valid_passport(const char *s) {
    if (s == NULL) {
        return 0;
    }

    size_t len = strlen(s);

    if (len == 0 || len > 9) {
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (!isupper(c) && !isdigit(c)) {
            return 0;
        }
    }

    return 1;
}

/*
 * Metatroph hmeromhnias apo string se time_t.
 * Morfh: YYYY-MM-DDTHH:MM
 * Paradeigma: 2026-07-01T08:00
 */
static time_t parse_datetime(const char *text) {
    struct tm tm_value;

    memset(&tm_value, 0, sizeof(tm_value));

    char *end = strptime(text, "%Y-%m-%dT%H:%M", &tm_value);

    if (end == NULL || *end != '\0') {
        return (time_t)-1;
    }

    tm_value.tm_isdst = -1;

    return mktime(&tm_value);
}

static Flight *find_flight_by_id(int id) {
    for (int i = 0; i < flight_count; i++) {
        if (flights[i].id == id) {
            return &flights[i];
        }
    }

    return NULL;
}

/*
 * Metraei poses theseis einai eleutheres.
 * Kaleitai otan exoume hdh kleidwsei to mutex ths pthshs.
 */
/*
 * Ftiaxnei string me ta noumera twn eleutherwn thesewn,
 * xwrismena me komma (p.x. "1,3,4,7").
 * Kaleitai otan exoume hdh kleidwsei to mutex ths pthshs.
 */
static void format_free_seats_unlocked(const Flight *f, char *out, size_t out_size) {
    out[0] = '\0';
    size_t used = 0;

    for (int s = 1; s <= f->total_seats; s++) {
        if (f->seats[s].occupied) {
            continue;
        }

        int written = snprintf(
            out + used,
            out_size - used,
            "%s%d",
            used == 0 ? "" : ",",
            s
        );

        if (written < 0 || (size_t)written >= out_size - used) {
            break;
        }

        used += (size_t)written;
    }
}

static int available_seats_unlocked(const Flight *f) {
    int available = 0;

    for (int s = 1; s <= f->total_seats; s++) {
        if (!f->seats[s].occupied) {
            available++;
        }
    }

    return available;
}

/*
 * Stelnei mia grammh apanthshs ston client.
 */
static void send_line(FILE *client, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(client, fmt, args);
    va_end(args);

    fprintf(client, "\n");
    fflush(client);
}

/*
 * Kleidwma 2 pthsewn me statherh seira.
 *
 * Giati to kanoume etsi:
 * Gia na apofygoume deadlock.
 *
 * An ena thread kleidwsei prwta thn pthsh 1 kai meta thn 2,
 * kai allo thread kleidwsei prwta thn 2 kai meta thn 1,
 * mporei na kollhsoun.
 *
 * Opote kleidwnoume panta prwta to mikrotero flight id.
 */
static void lock_two_flights(Flight *a, Flight *b) {
    if (a == b) {
        pthread_mutex_lock(&a->lock);
        return;
    }

    if (a->id < b->id) {
        pthread_mutex_lock(&a->lock);
        pthread_mutex_lock(&b->lock);
    } else {
        pthread_mutex_lock(&b->lock);
        pthread_mutex_lock(&a->lock);
    }
}

static void unlock_two_flights(Flight *a, Flight *b) {
    if (a == b) {
        pthread_mutex_unlock(&a->lock);
        return;
    }

    pthread_mutex_unlock(&a->lock);
    pthread_mutex_unlock(&b->lock);
}

/*
 * Grafei mia krathsh sto reservations.csv.
 *
 * To arxeio einai koino gia ola ta threads.
 * Ara to prostateuoume me reservation_file_mutex.
 */
static int append_reservation_file(
    const Passenger *p,
    int f1,
    int seat1,
    int f2,
    int seat2,
    int *reservation_id_out
) {
    int ok = 0;

    pthread_mutex_lock(&reservation_file_mutex);

    int rid = next_reservation_id++;

    FILE *fp = fopen(RESERVATIONS_FILE, "a");

    if (fp != NULL) {
        int written = fprintf(
            fp,
            "%d|%s|%s|%s|%d|%d|%d|%d\n",
            rid,
            p->passport,
            p->country,
            p->full_name,
            f1,
            seat1,
            f2,
            seat2
        );

        if (written > 0 && fflush(fp) == 0) {
            ok = 1;
            *reservation_id_out = rid;
        }

        fclose(fp);
    }

    pthread_mutex_unlock(&reservation_file_mutex);

    return ok;
}

/*
 * Shmadeuei mia thesh ws krathmenh sth mnhmh.
 * Kaleitai mono otan to mutex ths pthshs einai hdh kleidwmeno.
 */
static void mark_reserved_unlocked(
    Flight *f,
    int seat_no,
    const Passenger *p,
    int reservation_id
) {
    f->seats[seat_no].occupied = 1;
    f->seats[seat_no].reservation_id = reservation_id;
    f->seats[seat_no].passenger = *p;
}

/*
 * Entolh SEARCH.
 *
 * O client stelnei:
 * SEARCH|from|to|start|end
 *
 * O server epistrefei apeutheias pthseis kai pthseis me mia antapokrish.
 */
static void handle_search(FILE *client, char **tokens, int ntokens) {
    if (ntokens != 5) {
        send_line(client, "BEGIN");
        send_line(client, "ERR|Usage: SEARCH|from|to|start|end");
        send_line(client, "END");
        return;
    }

    const char *from = tokens[1];
    const char *to = tokens[2];

    time_t start = parse_datetime(tokens[3]);
    time_t end = parse_datetime(tokens[4]);

    if (start == (time_t)-1 || end == (time_t)-1 || end < start) {
        send_line(client, "BEGIN");
        send_line(client, "ERR|Invalid date interval");
        send_line(client, "END");
        return;
    }

    send_line(client, "BEGIN");

    /*
     * Psaxnoume apeutheias pthseis.
     */
    for (int i = 0; i < flight_count; i++) {
        Flight *f = &flights[i];

        if (
            strcmp(f->from, from) == 0 &&
            strcmp(f->to, to) == 0 &&
            f->depart_ts >= start &&
            f->arrive_ts <= end
        ) {
            pthread_mutex_lock(&f->lock);

            int available = available_seats_unlocked(f);

            send_line(
                client,
                "DIRECT|flight=%d|%s->%s|depart=%s|arrive=%s|available=%d",
                f->id,
                f->from,
                f->to,
                f->depart_str,
                f->arrive_str,
                available
            );

            pthread_mutex_unlock(&f->lock);
        }
    }

    /*
     * Psaxnoume pthseis me mia antapokrish.
     * Theloume:
     * a.from == from
     * a.to == b.from
     * b.to == to
     */
    for (int i = 0; i < flight_count; i++) {
        Flight *a = &flights[i];

        if (strcmp(a->from, from) != 0) {
            continue;
        }

        if (a->depart_ts < start) {
            continue;
        }

        for (int j = 0; j < flight_count; j++) {
            if (i == j) {
                continue;
            }

            Flight *b = &flights[j];

            if (strcmp(a->to, b->from) != 0) {
                continue;
            }

            if (strcmp(b->to, to) != 0) {
                continue;
            }

            if (b->arrive_ts > end) {
                continue;
            }

            double wait_minutes = difftime(b->depart_ts, a->arrive_ts) / 60.0;

            if (wait_minutes < MIN_CONNECTION_MINUTES) {
                continue;
            }

            if (wait_minutes > MAX_CONNECTION_HOURS * 60.0) {
                continue;
            }

            lock_two_flights(a, b);

            int available_a = available_seats_unlocked(a);
            int available_b = available_seats_unlocked(b);

            send_line(
                client,
                "CONNECT|flights=%d+%d|route=%s->%s->%s|first=%s/%s|second=%s/%s|available=%d,%d",
                a->id,
                b->id,
                a->from,
                a->to,
                b->to,
                a->depart_str,
                a->arrive_str,
                b->depart_str,
                b->arrive_str,
                available_a,
                available_b
            );

            unlock_two_flights(a, b);
        }
    }

    send_line(client, "END");
}

/*
 * Entolh SEATS.
 *
 * O client stelnei:
 * SEATS|flight_id
 *
 * Epistrefei tis eleutheres theseis ths sygkekrimenhs pthshs,
 * gia na kserei o xrhsths poion arithmo na dialeksei prin klhsei.
 */
static void handle_seats(FILE *client, char **tokens, int ntokens) {
    if (ntokens != 2) {
        send_line(client, "ERR|Usage: SEATS|flight_id");
        return;
    }

    int flight_id = atoi(tokens[1]);

    Flight *f = find_flight_by_id(flight_id);

    if (f == NULL) {
        send_line(client, "ERR|Flight not found");
        return;
    }

    pthread_mutex_lock(&f->lock);

    int available = available_seats_unlocked(f);

    char free_seats[1024];
    format_free_seats_unlocked(f, free_seats, sizeof(free_seats));

    send_line(
        client,
        "SEATS|flight=%d|available=%d|free_seats=%s",
        f->id,
        available,
        free_seats
    );

    pthread_mutex_unlock(&f->lock);
}

/*
 * Entolh BOOK_DIRECT.
 *
 * O client stelnei:
 * BOOK_DIRECT|flight_id|seat|passport|country|full_name
 */
static void handle_book_direct(FILE *client, char **tokens, int ntokens) {
    if (ntokens != 6) {
        send_line(client, "ERR|Usage: BOOK_DIRECT|flight_id|seat|passport|country|full_name");
        return;
    }

    int flight_id = atoi(tokens[1]);
    int seat_no = atoi(tokens[2]);

    Passenger p;

    snprintf(p.passport, sizeof(p.passport), "%s", tokens[3]);
    snprintf(p.country, sizeof(p.country), "%s", tokens[4]);
    snprintf(p.full_name, sizeof(p.full_name), "%s", tokens[5]);

    if (
        has_forbidden_delimiter(p.passport) ||
        has_forbidden_delimiter(p.country) ||
        has_forbidden_delimiter(p.full_name)
    ) {
        send_line(client, "ERR|Fields must not contain '|'");
        return;
    }

    if (!is_valid_passport(p.passport)) {
        send_line(client, "ERR|Invalid passport: max 9 chars, only uppercase letters/digits, no spaces");
        return;
    }

    Flight *f = find_flight_by_id(flight_id);

    if (f == NULL) {
        send_line(client, "ERR|Flight not found");
        return;
    }

    if (seat_no < 1 || seat_no > f->total_seats) {
        send_line(client, "ERR|Invalid seat number");
        return;
    }

    /*
     * Critical section.
     *
     * Edw ginontai mazi:
     * 1. Elegxos an h thesh einai eleutherh
     * 2. Eggrafh krathshs sto arxeio
     * 3. Enhmerwsh ths theshs sth mnhmh
     *
     * Xwris mutex, 2 clients tha mporousan na kratissoun thn idia thesh.
     */
    pthread_mutex_lock(&f->lock);

    if (f->seats[seat_no].occupied) {
        pthread_mutex_unlock(&f->lock);
        send_line(client, "ERR|Seat already reserved");
        return;
    }

    int rid = -1;

    if (!append_reservation_file(&p, flight_id, seat_no, 0, 0, &rid)) {
        pthread_mutex_unlock(&f->lock);
        send_line(client, "ERR|Could not write reservation file");
        return;
    }

    mark_reserved_unlocked(f, seat_no, &p, rid);

    pthread_mutex_unlock(&f->lock);

    send_line(client, "OK|reservation_id=%d", rid);
}

/*
 * Elegxei an oi pthseis a kai b apoteloun egkyrh antapokrish:
 * diaforetikes pthseis, idio endiameso aerodromio,
 * kai xroniko parathyro anamesa se MIN_CONNECTION_MINUTES
 * kai MAX_CONNECTION_HOURS.
 *
 * Epistrefei NULL an einai egkyrh, alliws minyma sfalmatos.
 */
static const char *validate_connection(const Flight *a, const Flight *b) {
    if (a == b) {
        return "Connecting reservation needs two different flights";
    }

    if (strcmp(a->to, b->from) != 0) {
        return "Flights do not form a valid connection";
    }

    double wait_minutes = difftime(b->depart_ts, a->arrive_ts) / 60.0;

    if (
        wait_minutes < MIN_CONNECTION_MINUTES ||
        wait_minutes > MAX_CONNECTION_HOURS * 60.0
    ) {
        return "Connection time is not valid";
    }

    return NULL;
}

/*
 * Entolh CHECK_CONNECT.
 *
 * O client stelnei:
 * CHECK_CONNECT|flight1_id|flight2_id
 *
 * Xrhsimopoieitai apo ton client PRIN zhthsei seats/passport/onoma,
 * gia na enhmerwsei norwis an oi 2 pthseis den apotelun egkyrh
 * antapokrish, xwris na xreiastei na ginei olokliri h BOOK_CONNECT.
 */
static void handle_check_connect(FILE *client, char **tokens, int ntokens) {
    if (ntokens != 3) {
        send_line(client, "ERR|Usage: CHECK_CONNECT|flight1_id|flight2_id");
        return;
    }

    int flight1_id = atoi(tokens[1]);
    int flight2_id = atoi(tokens[2]);

    Flight *a = find_flight_by_id(flight1_id);
    Flight *b = find_flight_by_id(flight2_id);

    if (a == NULL || b == NULL) {
        send_line(client, "ERR|One or both flights not found");
        return;
    }

    const char *connect_err = validate_connection(a, b);

    if (connect_err != NULL) {
        send_line(client, "ERR|%s", connect_err);
        return;
    }

    send_line(client, "OK|Valid connection");
}

/*
 * Entolh HAS_CONNECTIONS.
 *
 * O client stelnei:
 * HAS_CONNECTIONS|flight1_id
 *
 * Elegxei an yparxei ESTW MIA alli pthsh pou na apotelei
 * egkyrh antapokrish me thn flight1_id. Xrhsimopoieitai apo ton
 * client amesws meta thn epilogh ths prwths pthshs, gia na
 * enhmerwsei norwis an den yparxei kammia dynath antapokrish -
 * xwris na xreiastei na dokimazei o xrhsths flight ids sto tyflo.
 */
static void handle_has_connections(FILE *client, char **tokens, int ntokens) {
    if (ntokens != 2) {
        send_line(client, "ERR|Usage: HAS_CONNECTIONS|flight1_id");
        return;
    }

    int flight1_id = atoi(tokens[1]);

    Flight *a = find_flight_by_id(flight1_id);

    if (a == NULL) {
        send_line(client, "ERR|Flight not found");
        return;
    }

    for (int i = 0; i < flight_count; i++) {
        Flight *b = &flights[i];

        if (validate_connection(a, b) == NULL) {
            send_line(client, "OK|Has connections");
            return;
        }
    }

    send_line(client, "ERR|No connecting flights depart from %s after this flight arrives", a->to);
}

/*
 * Entolh BOOK_CONNECT.
 *
 * O client stelnei:
 * BOOK_CONNECT|flight1_id|seat1|flight2_id|seat2|passport|country|full_name
 *
 * Edw prepei na ginei all-or-nothing.
 * Dhladh eite kratountai kai oi dyo theseis,
 * eite den kratietai kamia.
 */
static void handle_book_connect(FILE *client, char **tokens, int ntokens) {
    if (ntokens != 8) {
        send_line(client, "ERR|Usage: BOOK_CONNECT|flight1_id|seat1|flight2_id|seat2|passport|country|full_name");
        return;
    }

    int flight1_id = atoi(tokens[1]);
    int seat1 = atoi(tokens[2]);

    int flight2_id = atoi(tokens[3]);
    int seat2 = atoi(tokens[4]);

    Passenger p;

    snprintf(p.passport, sizeof(p.passport), "%s", tokens[5]);
    snprintf(p.country, sizeof(p.country), "%s", tokens[6]);
    snprintf(p.full_name, sizeof(p.full_name), "%s", tokens[7]);

    if (
        has_forbidden_delimiter(p.passport) ||
        has_forbidden_delimiter(p.country) ||
        has_forbidden_delimiter(p.full_name)
    ) {
        send_line(client, "ERR|Fields must not contain '|'");
        return;
    }

    if (!is_valid_passport(p.passport)) {
        send_line(client, "ERR|Invalid passport: max 9 chars, only uppercase letters/digits, no spaces");
        return;
    }

    Flight *a = find_flight_by_id(flight1_id);
    Flight *b = find_flight_by_id(flight2_id);

    if (a == NULL || b == NULL) {
        send_line(client, "ERR|One or both flights not found");
        return;
    }

    const char *connect_err = validate_connection(a, b);

    if (connect_err != NULL) {
        send_line(client, "ERR|%s", connect_err);
        return;
    }

    if (
        seat1 < 1 ||
        seat1 > a->total_seats ||
        seat2 < 1 ||
        seat2 > b->total_seats
    ) {
        send_line(client, "ERR|Invalid seat number");
        return;
    }

    /*
     * Kleidwnoume kai tis 2 pthseis.
     * Xrhsimopoioume statherh seira gia na mhn exoume deadlock.
     */
    lock_two_flights(a, b);

    /*
     * All-or-nothing.
     * An mia apo tis dyo theseis einai piasmenh,
     * den kratame kamia.
     */
    if (a->seats[seat1].occupied || b->seats[seat2].occupied) {
        unlock_two_flights(a, b);
        send_line(client, "ERR|At least one seat is already reserved; nothing was booked");
        return;
    }

    int rid = -1;

    if (!append_reservation_file(&p, a->id, seat1, b->id, seat2, &rid)) {
        unlock_two_flights(a, b);
        send_line(client, "ERR|Could not write reservation file; nothing was booked");
        return;
    }

    mark_reserved_unlocked(a, seat1, &p, rid);
    mark_reserved_unlocked(b, seat2, &p, rid);

    unlock_two_flights(a, b);

    send_line(client, "OK|reservation_id=%d", rid);
}

/*
 * Spaei mia grammh se kommatia me diaxwristiko to |.
 */
static int split_tokens(char *line, char **tokens, int max_tokens) {
    int count = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(line, "|", &saveptr);

    while (tok != NULL && count < max_tokens) {
        trim_spaces(tok);
        tokens[count++] = tok;
        tok = strtok_r(NULL, "|", &saveptr);
    }

    return count;
}

/*
 * Auth einai h synarthsh pou trexei kathe thread.
 * Kathe client exei diko tou thread.
 */
static void *client_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    FILE *client = fdopen(client_fd, "r+");

    if (client == NULL) {
        close(client_fd);
        return NULL;
    }

    setvbuf(client, NULL, _IOLBF, 0);

    send_line(client, "OK|Connected to airline reservation server");

    char line[BUFFER_SIZE];

    while (fgets(line, sizeof(line), client) != NULL) {
        trim_newline(line);

        if (line[0] == '\0') {
            continue;
        }

        char copy[BUFFER_SIZE];
        snprintf(copy, sizeof(copy), "%s", line);

        char *tokens[16];
        int ntokens = split_tokens(copy, tokens, 16);

        if (ntokens == 0) {
            continue;
        }

        if (strcmp(tokens[0], CMD_QUIT) == 0) {
            send_line(client, "OK|Bye");
            break;
        } else if (strcmp(tokens[0], CMD_SEARCH) == 0) {
            handle_search(client, tokens, ntokens);
        } else if (strcmp(tokens[0], CMD_SEATS) == 0) {
            handle_seats(client, tokens, ntokens);
        } else if (strcmp(tokens[0], CMD_CHECK_CONNECT) == 0) {
            handle_check_connect(client, tokens, ntokens);
        } else if (strcmp(tokens[0], CMD_HAS_CONNECTIONS) == 0) {
            handle_has_connections(client, tokens, ntokens);
        } else if (strcmp(tokens[0], CMD_BOOK_DIRECT) == 0) {
            handle_book_direct(client, tokens, ntokens);
        } else if (strcmp(tokens[0], CMD_BOOK_CONNECT) == 0) {
            handle_book_connect(client, tokens, ntokens);
        } else {
            send_line(client, "ERR|Unknown command");
        }
    }

    fclose(client);

    return NULL;
}

static void ensure_reservations_file_exists(void) {
    FILE *fp = fopen(RESERVATIONS_FILE, "a");

    if (fp == NULL) {
        die("fopen reservations.csv");
    }

    fclose(fp);
}

/*
 * Fortwnei tis pthseis apo to flights.csv.
 */
static void load_flights(void) {
    FILE *fp = fopen(FLIGHTS_FILE, "r");

    if (fp == NULL) {
        die("fopen flights.csv");
    }

    char line[BUFFER_SIZE];

    while (fgets(line, sizeof(line), fp) != NULL) {
        trim_newline(line);

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        if (flight_count >= MAX_FLIGHTS) {
            fprintf(stderr, "Too many flights\n");
            exit(EXIT_FAILURE);
        }

        char *saveptr = NULL;

        char *id_s = strtok_r(line, ",", &saveptr);
        char *from_s = strtok_r(NULL, ",", &saveptr);
        char *to_s = strtok_r(NULL, ",", &saveptr);
        char *dep_s = strtok_r(NULL, ",", &saveptr);
        char *arr_s = strtok_r(NULL, ",", &saveptr);
        char *seats_s = strtok_r(NULL, ",", &saveptr);

        if (
            id_s == NULL ||
            from_s == NULL ||
            to_s == NULL ||
            dep_s == NULL ||
            arr_s == NULL ||
            seats_s == NULL
        ) {
            fprintf(stderr, "Bad line in flights.csv\n");
            continue;
        }

        trim_spaces(id_s);
        trim_spaces(from_s);
        trim_spaces(to_s);
        trim_spaces(dep_s);
        trim_spaces(arr_s);
        trim_spaces(seats_s);

        Flight *f = &flights[flight_count];

        memset(f, 0, sizeof(*f));

        f->id = atoi(id_s);

        snprintf(f->from, sizeof(f->from), "%s", from_s);
        snprintf(f->to, sizeof(f->to), "%s", to_s);
        snprintf(f->depart_str, sizeof(f->depart_str), "%s", dep_s);
        snprintf(f->arrive_str, sizeof(f->arrive_str), "%s", arr_s);

        f->depart_ts = parse_datetime(f->depart_str);
        f->arrive_ts = parse_datetime(f->arrive_str);

        f->total_seats = atoi(seats_s);

        if (
            f->id <= 0 ||
            f->depart_ts == (time_t)-1 ||
            f->arrive_ts == (time_t)-1 ||
            f->arrive_ts <= f->depart_ts ||
            f->total_seats < 1 ||
            f->total_seats > MAX_SEATS
        ) {
            fprintf(stderr, "Invalid flight ignored\n");
            continue;
        }

        /*
         * Arxikopoihsh mutex gia auth thn pthsh.
         */
        pthread_mutex_init(&f->lock, NULL);

        flight_count++;
    }

    fclose(fp);

    printf("Loaded %d flights.\n", flight_count);
}

/*
 * Fortwnei tis yparxouses krathseis apo to reservations.csv.
 */
static void load_reservations(void) {
    ensure_reservations_file_exists();

    FILE *fp = fopen(RESERVATIONS_FILE, "r");

    if (fp == NULL) {
        die("fopen reservations.csv");
    }

    char line[BUFFER_SIZE];

    while (fgets(line, sizeof(line), fp) != NULL) {
        trim_newline(line);

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *tokens[8];
        int ntokens = split_tokens(line, tokens, 8);

        if (ntokens != 8) {
            continue;
        }

        int rid = atoi(tokens[0]);

        Passenger p;

        snprintf(p.passport, sizeof(p.passport), "%s", tokens[1]);
        snprintf(p.country, sizeof(p.country), "%s", tokens[2]);
        snprintf(p.full_name, sizeof(p.full_name), "%s", tokens[3]);

        int f1_id = atoi(tokens[4]);
        int seat1 = atoi(tokens[5]);
        int f2_id = atoi(tokens[6]);
        int seat2 = atoi(tokens[7]);

        Flight *f1 = find_flight_by_id(f1_id);

        if (
            f1 != NULL &&
            seat1 >= 1 &&
            seat1 <= f1->total_seats &&
            !f1->seats[seat1].occupied
        ) {
            mark_reserved_unlocked(f1, seat1, &p, rid);
        }

        if (f2_id > 0) {
            Flight *f2 = find_flight_by_id(f2_id);

            if (
                f2 != NULL &&
                seat2 >= 1 &&
                seat2 <= f2->total_seats &&
                !f2->seats[seat2].occupied
            ) {
                mark_reserved_unlocked(f2, seat2, &p, rid);
            }
        }

        if (rid >= next_reservation_id) {
            next_reservation_id = rid + 1;
        }
    }

    fclose(fp);

    printf("Loaded reservations. Next reservation id: %d.\n", next_reservation_id);
}

/*
 * Dhmioyrgei TCP socket gia ton server.
 */
static int create_server_socket(int port) {
    int server_fd = socket(AF_INET6, SOCK_STREAM, 0);

    if (server_fd < 0) {
        die("socket");
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        die("setsockopt");
    }

    struct sockaddr_in6 addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("bind");
    }

    if (listen(server_fd, 32) < 0) {
        die("listen");
    }

    return server_fd;
}

int main(int argc, char **argv) {
    /*
     * Agnooume SIGPIPE gia na mhn pesei o server
     * an kapoios client kleisei apotoma.
     */
    signal(SIGPIPE, SIG_IGN);

    int port = DEFAULT_PORT;

    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    load_flights();
    load_reservations();

    int server_fd = create_server_socket(port);

    printf("Server listening on port %d...\n", port);

    /*
     * O server trexei synexeia.
     * Gia kathe neo client dhmiourgei neo thread.
     */
    while (1) {
        struct sockaddr_in6 client_addr;
        socklen_t len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            continue;
        }

        int *arg = malloc(sizeof(int));

        if (arg == NULL) {
            close(client_fd);
            continue;
        }

        *arg = client_fd;

        pthread_t tid;

        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(arg);
            continue;
        }

        /*
         * Den kanoume pthread_join giati o server trexei synexeia.
         * Me pthread_detach to thread katharizetai mono tou.
         */
        pthread_detach(tid);
    }

    close(server_fd);

    return EXIT_SUCCESS;
}