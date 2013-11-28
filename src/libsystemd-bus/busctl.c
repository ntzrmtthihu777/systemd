/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <getopt.h>

#include "strv.h"
#include "util.h"
#include "log.h"
#include "build.h"
#include "pager.h"

#include "sd-bus.h"
#include "bus-message.h"
#include "bus-internal.h"
#include "bus-util.h"
#include "bus-dump.h"

static bool arg_no_pager = false;
static char *arg_address = NULL;
static bool arg_no_unique = false;
static bool arg_no_machine = false;
static char **arg_matches = NULL;
static BusTransport arg_transport = BUS_TRANSPORT_LOCAL;
static char *arg_host = NULL;
static bool arg_user = false;

static void pager_open_if_enabled(void) {

        /* Cache result before we open the pager */
        if (arg_no_pager)
                return;

        pager_open(false);
}

static int list_bus_names(sd_bus *bus, char **argv) {
        _cleanup_strv_free_ char **l = NULL;
        char **i;
        int r;
        size_t max_i = 0;

        assert(bus);

        r = sd_bus_list_names(bus, &l);
        if (r < 0) {
                log_error("Failed to list names: %s", strerror(-r));
                return r;
        }

        pager_open_if_enabled();

        strv_sort(l);

        STRV_FOREACH(i, l)
                max_i = MAX(max_i, strlen(*i));

        printf("%-*s %*s %-*s %-*s %-*s",
               (int) max_i, "NAME", 10, "PID", 15, "PROCESS", 16, "USER", 20, "CONNECTION");

        if (!arg_no_machine)
                puts(" MACHINE");
        else
                putchar('\n');

        STRV_FOREACH(i, l) {
                _cleanup_bus_creds_unref_ sd_bus_creds *creds = NULL;
                _cleanup_free_ char *owner = NULL;
                sd_id128_t mid;

                if (arg_no_unique && (*i)[0] == ':')
                        continue;

                printf("%-*s", (int) max_i, *i);

                r = sd_bus_get_owner(bus, *i, SD_BUS_CREDS_UID|SD_BUS_CREDS_PID|SD_BUS_CREDS_COMM, &owner, &creds);
                if (r >= 0) {
                        pid_t pid;
                        uid_t uid;

                        r = sd_bus_creds_get_pid(creds, &pid);
                        if (r >= 0) {
                                const char *comm = NULL;

                                sd_bus_creds_get_comm(creds, &comm);

                                printf(" %10lu %-15s", (unsigned long) pid, strna(comm));
                        } else
                                fputs("          - -              ", stdout);

                        r = sd_bus_creds_get_uid(creds, &uid);
                        if (r >= 0) {
                                _cleanup_free_ char *u = NULL;

                                u = uid_to_name(uid);
                                if (!u)
                                        return log_oom();

                                if (strlen(u) > 16)
                                        u[16] = 0;

                                printf(" %-16s", u);
                        } else
                                fputs(" -               ", stdout);

                        if (owner)
                                printf(" %-20s", owner);
                        else
                                fputs(" -                   ", stdout);

                } else
                        printf("          - -               -                -                   ");

                if (arg_no_machine)
                        putchar('\n');
                else {
                        r = sd_bus_get_owner_machine_id(bus, *i, &mid);
                        if (r >= 0) {
                                char m[SD_ID128_STRING_MAX];
                                printf(" %s\n", sd_id128_to_string(mid, m));
                        } else
                                puts(" -");
                }
        }

        return 0;
}

static int monitor(sd_bus *bus, char *argv[]) {
        bool added_something = false;
        char **i;
        int r;

        STRV_FOREACH(i, argv+1) {
                _cleanup_free_ char *m = NULL;

                if (!service_name_is_valid(*i)) {
                        log_error("Invalid service name '%s'", *i);
                        return -EINVAL;
                }

                m = strjoin("sender='", *i, "'", NULL);
                if (!m)
                        return log_oom();

                r = sd_bus_add_match(bus, m, NULL, NULL);
                if (r < 0) {
                        log_error("Failed to add match: %s", strerror(-r));
                        return r;
                }

                added_something = true;
        }

        STRV_FOREACH(i, arg_matches) {
                r = sd_bus_add_match(bus, *i, NULL, NULL);
                if (r < 0) {
                        log_error("Failed to add match: %s", strerror(-r));
                        return r;
                }

                added_something = true;
        }

        if (!added_something) {
                r = sd_bus_add_match(bus, "", NULL, NULL);
                if (r < 0) {
                        log_error("Failed to add match: %s", strerror(-r));
                        return r;
                }
        }

        for (;;) {
                _cleanup_bus_message_unref_ sd_bus_message *m = NULL;

                r = sd_bus_process(bus, &m);
                if (r < 0) {
                        log_error("Failed to process bus: %s", strerror(-r));
                        return r;
                }

                if (m) {
                        bus_message_dump(m, stdout, true);
                        continue;
                }

                if (r > 0)
                        continue;

                r = sd_bus_wait(bus, (uint64_t) -1);
                if (r < 0) {
                        log_error("Failed to wait for bus: %s", strerror(-r));
                        return r;
                }
        }

        return -EINVAL;
}

static int help(void) {

        printf("%s [OPTIONS...] {COMMAND} ...\n\n"
               "Introspect the bus.\n\n"
               "  -h --help               Show this help\n"
               "     --version            Show package version\n"
               "     --no-pager           Do not pipe output into a pager\n"
               "     --system             Connect to system bus\n"
               "     --user               Connect to user bus\n"
               "  -H --host=[USER@]HOST   Operate on remote host\n"
               "  -M --machine=CONTAINER  Operate on local container\n"
               "     --address=ADDRESS    Connect to bus specified by address\n"
               "     --no-unique          Only show well-known names\n"
               "     --no-machine         Don't show machine ID column in list\n\n"
               "     --match=MATCH        Only show matching messages\n"
               "Commands:\n"
               "  list                    List bus names\n"
               "  monitor [SERVICE...]    Show bus traffic\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_SYSTEM,
                ARG_USER,
                ARG_ADDRESS,
                ARG_MATCH,
                ARG_NO_UNIQUE,
                ARG_NO_MACHINE,
        };

        static const struct option options[] = {
                { "help",       no_argument,       NULL, 'h'            },
                { "version",    no_argument,       NULL, ARG_VERSION    },
                { "no-pager",   no_argument,       NULL, ARG_NO_PAGER   },
                { "system",     no_argument,       NULL, ARG_SYSTEM     },
                { "user",       no_argument,       NULL, ARG_USER       },
                { "address",    required_argument, NULL, ARG_ADDRESS    },
                { "no-unique",  no_argument,       NULL, ARG_NO_UNIQUE  },
                { "no-machine", no_argument,       NULL, ARG_NO_MACHINE },
                { "match",      required_argument, NULL, ARG_MATCH      },
                { "host",       required_argument, NULL, 'H'            },
                { "machine",    required_argument, NULL, 'M'            },
                {},
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hH:M:", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case ARG_USER:
                        arg_user = true;
                        break;

                case ARG_SYSTEM:
                        arg_user = false;
                        break;

                case ARG_ADDRESS:
                        arg_address = optarg;
                        break;

                case ARG_NO_UNIQUE:
                        arg_no_unique = true;
                        break;

                case ARG_NO_MACHINE:
                        arg_no_machine = true;
                        break;

                case ARG_MATCH:
                        if (strv_extend(&arg_matches, optarg) < 0)
                                return log_oom();
                        break;

                case 'H':
                        arg_transport = BUS_TRANSPORT_REMOTE;
                        arg_host = optarg;
                        break;

                case 'M':
                        arg_transport = BUS_TRANSPORT_CONTAINER;
                        arg_host = optarg;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

static int busctl_main(sd_bus *bus, int argc, char *argv[]) {
        assert(bus);

        if (optind >= argc ||
            streq(argv[optind], "list"))
                return list_bus_names(bus, argv + optind);

        if (streq(argv[optind], "monitor"))
                return monitor(bus, argv + optind);

        if (streq(argv[optind], "help"))
                return help();

        log_error("Unknown command '%s'", argv[optind]);
        return -EINVAL;
}

int main(int argc, char *argv[]) {
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (arg_address) {
                r = sd_bus_new(&bus);
                if (r < 0) {
                        log_error("Failed to allocate bus: %s", strerror(-r));
                        goto finish;
                }

                r = sd_bus_set_address(bus, arg_address);
                if (r < 0) {
                        log_error("Failed to set address: %s", strerror(-r));
                        goto finish;
                }

                r = sd_bus_set_bus_client(bus, true);
                if (r < 0) {
                        log_error("Failed to set bus client: %s", strerror(-r));
                        goto finish;
                }

                r = sd_bus_start(bus);
        } else
                r = bus_open_transport(arg_transport, arg_host, arg_user, &bus);

        if (r < 0) {
                log_error("Failed to connect to bus: %s", strerror(-r));
                goto finish;
        }

        r = busctl_main(bus, argc, argv);

finish:
        pager_close();

        strv_free(arg_matches);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
