#define _XOPEN_SOURCE 700

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

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

/*
 * Diavazei eisodo apo ton xrhsth.
 */
static void read_input(const char *prompt, char *buffer, size_t size) {
    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buffer, size, stdin) == NULL) {
        printf("\n");
        exit(EXIT_SUCCESS);
    }

    trim_newline(buffer);
}

/*
 * Diavazei eisodo pou den epitrepetai na einai kenh.
 * Xanarwtaei mexri o xrhsths na dwsei kati.
 */
static void read_required_input(const char *prompt, char *buffer, size_t size) {
    while (1) {
        read_input(prompt, buffer, size);

        if (buffer[0] != '\0') {
            return;
        }

        printf("This field cannot be empty.\n");
    }
}

/*
 * Syndesh tou client ston server.
 */
static int connect_to_server(const char *host, int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in6 addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((uint16_t)port);

    if (inet_pton(AF_INET6, host, &addr.sin6_addr) <= 0) {
        fprintf(stderr, "Invalid IPv6 address. Example: ::1\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        exit(EXIT_FAILURE);
    }

    return fd;
}

/*
 * Diavazei hmerominia (xwris wra) apo ton xrhsth, elegxontas th
 * morfh prin stalei sto server.
 */
static void read_date(const char *label, char *buffer, size_t size) {
    char prompt[128];

    snprintf(
        prompt,
        sizeof(prompt),
        "%s (YYYY-MM-DD, e.g. 2026-07-01): ",
        label
    );

    while (1) {
        read_input(prompt, buffer, size);

        struct tm tm_value;

        memset(&tm_value, 0, sizeof(tm_value));

        char *end = strptime(buffer, "%Y-%m-%d", &tm_value);

        if (end == NULL || *end != '\0') {
            printf(
                "Invalid format. Use YYYY-MM-DD (e.g. 2026-07-01).\n"
            );
            continue;
        }

        return;
    }
}

/*
 * Idios kanonas me to server: to poly 9 xarakthres,
 * mono kefalaia grammata kai arithmous, xwris kena.
 * Ton elegxoume kai edw, sto client, gia na min xreiazetai
 * o xrhsths na sympliroei ta ypoloipa pedia an einai lathos.
 */
static int is_valid_passport(const char *s) {
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
 * Diavazei arithmo diavatiriou, xanarwtontas mexri na einai egkyros
 * (to poly 9 xarakthres, mono kefalaia grammata/arithmous, xwris kena).
 */
static void read_passport(const char *prompt, char *buffer, size_t size) {
    while (1) {
        read_input(prompt, buffer, size);

        if (is_valid_passport(buffer)) {
            return;
        }

        printf(
            "Invalid passport number. "
            "Only up to 9 characters, uppercase letters and digits, "
            "no spaces or special characters.\n"
        );
    }
}

/*
 * Gia SEARCH o server stelnei polla apotelesmata.
 * Arxizei me BEGIN kai teleiwnei me END.
 */
static void receive_until_end(FILE *server) {
    char line[BUFFER_SIZE];
    int result_count = 0;

    while (fgets(line, sizeof(line), server) != NULL) {
        trim_newline(line);

        if (strcmp(line, "END") == 0) {
            break;
        }

        if (strcmp(line, "BEGIN") == 0) {
            continue;
        }

        printf("%s\n", line);
        result_count++;
    }

    if (result_count == 0) {
        printf("No flights found for the given criteria.\n");
    }
}

/*
 * Gia QUIT o server stelnei mia grammh.
 */
static void receive_one_line(FILE *server) {
    char line[BUFFER_SIZE];

    if (fgets(line, sizeof(line), server) != NULL) {
        trim_newline(line);
        printf("%s\n", line);
    }
}

/*
 * Stelnei SEATS|flight_id kai typwnei thn apantisi.
 * Epistrefei 1 an itan sfalma (p.x. "Flight not found"),
 * gia na xanarwtisei o kaloun ton flight id xwris na synexisei
 * na zhtaei ta ypoloipa pedia ths krathshs.
 */
static int show_seats_and_check_error(FILE *server, const char *flight_id) {
    char line[BUFFER_SIZE];

    fprintf(server, "SEATS|%s\n", flight_id);
    fflush(server);

    if (fgets(line, sizeof(line), server) == NULL) {
        printf("No response received from the server.\n");
        return 1;
    }

    trim_newline(line);
    printf("%s\n", line);

    return strncmp(line, "ERR|", 4) == 0;
}

/*
 * Diavazei flight id, deixnei tis eleutheres theseis tou,
 * kai xanarwtaei an to flight id den yparxei -
 * xwris na zhtaei entwmetaxy ta ypoloipa pedia ths krathshs.
 */
static void read_flight_id_with_seats(
    FILE *server,
    const char *prompt,
    char *flight_id,
    size_t size
) {
    while (1) {
        read_input(prompt, flight_id, size);

        if (!show_seats_and_check_error(server, flight_id)) {
            return;
        }
    }
}

/*
 * Stelnei CHECK_CONNECT|flight1_id|flight2_id kai typwnei thn apantisi.
 * Epistrefei 1 an h antapokrish einai egkyrh.
 */
static int check_connection_valid(FILE *server, const char *f1, const char *f2) {
    char line[BUFFER_SIZE];

    fprintf(server, "CHECK_CONNECT|%s|%s\n", f1, f2);
    fflush(server);

    if (fgets(line, sizeof(line), server) == NULL) {
        printf("No response received from the server.\n");
        return 0;
    }

    trim_newline(line);
    printf("%s\n", line);

    return strncmp(line, "OK|", 3) == 0;
}

/*
 * Stelnei HAS_CONNECTIONS|flight1_id kai typwnei thn apantisi.
 * Epistrefei 1 an yparxei estw mia dynath antapokrish, alliws 0.
 * Xrhsimopoieitai amesws meta thn epilogh ths prwths pthshs, gia
 * na min zhtiethei seat/second flight id/passport otan den yparxei
 * kammia dynath antapokrish gia authn thn pthsh.
 */
static int has_any_connection(FILE *server, const char *f1) {
    char line[BUFFER_SIZE];

    fprintf(server, "HAS_CONNECTIONS|%s\n", f1);
    fflush(server);

    if (fgets(line, sizeof(line), server) == NULL) {
        printf("No response received from the server.\n");
        return 0;
    }

    trim_newline(line);

    if (strncmp(line, "ERR|", 4) == 0) {
        printf("%s\n", line + 4);
        return 0;
    }

    return 1;
}

/*
 * Diavazei to deytero flight id gia antapokrish. Deixnei tis
 * eleutheres theseis tou, kai elegxei epipleon oti ontws
 * apotelei egkyrh antapokrish me to prwto flight id - xanarwtontas
 * an den ischyei, xwris na zhtaei entwmetaxy ta ypoloipa pedia.
 */
static void read_second_flight_id(
    FILE *server,
    const char *prompt,
    const char *f1,
    char *f2,
    size_t size
) {
    while (1) {
        read_flight_id_with_seats(server, prompt, f2, size);

        if (check_connection_valid(server, f1, f2)) {
            return;
        }
    }
}

/*
 * Gia BOOK_DIRECT/BOOK_CONNECT o server stelnei mia grammh
 * ("OK|reservation_id=.." i "ERR|..").
 * Thn emfanizoume se pio kataliki morfi gia ton xrhsth.
 */
static void receive_booking_response(FILE *server) {
    char line[BUFFER_SIZE];

    if (fgets(line, sizeof(line), server) == NULL) {
        printf("No response received from the server (connection may have been lost).\n");
        return;
    }

    trim_newline(line);

    static const char ok_prefix[] = "OK|reservation_id=";
    static const char err_prefix[] = "ERR|";

    if (strncmp(line, ok_prefix, sizeof(ok_prefix) - 1) == 0) {
        printf("Booking successful! Reservation number: %s\n", line + sizeof(ok_prefix) - 1);
    } else if (strncmp(line, err_prefix, sizeof(err_prefix) - 1) == 0) {
        printf("Booking failed: %s\n", line + sizeof(err_prefix) - 1);
    } else {
        printf("%s\n", line);
    }
}

int main(int argc, char **argv) {
    const char *host = "::1";
    int port = DEFAULT_PORT;

    if (argc >= 2) {
        host = argv[1];
    }

    if (argc >= 3) {
        port = atoi(argv[2]);
    }

    int fd = connect_to_server(host, port);

    FILE *server = fdopen(fd, "r+");

    if (server == NULL) {
        perror("fdopen");
        close(fd);
        return EXIT_FAILURE;
    }

    setvbuf(server, NULL, _IOLBF, 0);

    printf("Connected. Server says: ");
    receive_one_line(server);

    while (1) {
        printf("\n--- Airline Client ---\n");
        printf("1. Search flights\n");
        printf("2. Book direct flight\n");
        printf("3. Book connecting flights\n");
        printf("4. Quit\n");

        char choice[16];

        read_input("Choice: ", choice, sizeof(choice));

        if (strcmp(choice, "1") == 0) {
            char from[64];
            char to[64];
            char start_date[16];
            char end_date[16];
            char start[32];
            char end[32];

            read_input("From city: ", from, sizeof(from));
            read_input("To city: ", to, sizeof(to));
            read_date("Start date", start_date, sizeof(start_date));
            read_date("End date", end_date, sizeof(end_date));

            snprintf(start, sizeof(start), "%sT00:00", start_date);
            snprintf(end, sizeof(end), "%sT23:59", end_date);

            fprintf(server, "SEARCH|%s|%s|%s|%s\n", from, to, start, end);
            fflush(server);

            receive_until_end(server);

        } else if (strcmp(choice, "2") == 0) {
            char flight_id[16];
            char seat[16];
            char passport[32];
            char country[32];
            char first_name[64];
            char last_name[64];
            char name[128];

            read_flight_id_with_seats(server, "Flight id: ", flight_id, sizeof(flight_id));

            read_input("Seat number: ", seat, sizeof(seat));
            read_passport("Passport number: ", passport, sizeof(passport));
            read_input("Country: ", country, sizeof(country));
            read_required_input("First name: ", first_name, sizeof(first_name));
            read_required_input("Last name: ", last_name, sizeof(last_name));
            snprintf(name, sizeof(name), "%s %s", first_name, last_name);

            fprintf(
                server,
                "BOOK_DIRECT|%s|%s|%s|%s|%s\n",
                flight_id,
                seat,
                passport,
                country,
                name
            );

            fflush(server);

            receive_booking_response(server);

        } else if (strcmp(choice, "3") == 0) {
            char f1[16];
            char s1[16];
            char f2[16];
            char s2[16];
            char passport[32];
            char country[32];
            char first_name[64];
            char last_name[64];
            char name[128];

            read_flight_id_with_seats(server, "First flight id: ", f1, sizeof(f1));

            if (!has_any_connection(server, f1)) {
                continue;
            }

            read_input("Seat on first flight: ", s1, sizeof(s1));

            read_second_flight_id(server, "Second flight id: ", f1, f2, sizeof(f2));

            read_input("Seat on second flight: ", s2, sizeof(s2));
            read_passport("Passport number: ", passport, sizeof(passport));
            read_input("Country: ", country, sizeof(country));
            read_required_input("First name: ", first_name, sizeof(first_name));
            read_required_input("Last name: ", last_name, sizeof(last_name));
            snprintf(name, sizeof(name), "%s %s", first_name, last_name);

            fprintf(
                server,
                "BOOK_CONNECT|%s|%s|%s|%s|%s|%s|%s\n",
                f1,
                s1,
                f2,
                s2,
                passport,
                country,
                name
            );

            fflush(server);

            receive_booking_response(server);

        } else if (strcmp(choice, "4") == 0) {
            fprintf(server, "QUIT\n");
            fflush(server);

            receive_one_line(server);
            break;

        } else {
            printf("Invalid choice.\n");
        }
    }

    fclose(server);

    return EXIT_SUCCESS;
}