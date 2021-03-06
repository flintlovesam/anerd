/*

anerd-udp: a Network Exchange Randomness Daemon

  Copyright (C) 2012-2013 Dustin Kirkland <dustin.kirkland@gmail.com>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published by
  the Free Software Foundation, version 3 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_EXCHANGE_SIZE 64
#define DEFAULT_DEVICE "/dev/urandom"
#define DEFAULT_PORT 26373
#define DAEMON_USER "daemon"

/* Daemonize process properly */
void daemonize();

/*
anerd salt():
 - Generate a current local timestamp.
 - Chain salts together over time, causing recreation to be increasingly difficult
   over time.
*/
uint64_t anerd_salt(uint64_t salt) {
	struct timeval tv;
	struct timezone tz;
	static int init = 0;
	/* Update local timestamp, generate new salt */
	if (!init) {
		gettimeofday(&tv, &tz);
		/* XOR data together, chaining and mixing salt over time */
		srandom(1000000 * tv.tv_sec + tv.tv_usec);
		init++;
	}
	salt ^= random();
	return salt;
}

/*
anerd crc:
 - Create very simplistic checksum, adding the integer value of each byte.
 - Useful when debugging, identifying exchanged packets.
*/
int anerd_crc(char *data, int len) {
	int i = 0;
	int crc = 0;
	for (i=0; i<len; i++) {
		crc += (unsigned char)data[i];
	}
	return crc;
}

/*
anerd server:
 - Listen for communications on a UDP socket.
 - Take any received input, salt with some local randomness.
 - Add to the entropy pool.
 - Transmit back to the initiator the same number of bytes of randomness.
*/
int anerd_server(char *device, int size, int port, int ipv6) {
	int sock;
	int addr_len = sizeof(struct sockaddr);
	int bytes_read;
	uint64_t salt;
	char *data;
	FILE *fp;
	/* Structures for ipv6 */
	struct sockaddr_in6 server_addr6;
	struct ipv6_mreq imreq;
	/* Structures for ipv4 */
	struct sockaddr_in server_addr, client_addr;
	/* Open the random device */
	if ((fp = fopen(device, "a+")) == NULL) {
		syslog(LOG_ERR, "ERROR: fopen");
		exit(1);
	}
	/* Allocate and zero a data buffer to the chosen size */
	if ((data = calloc(size + 1, sizeof(char))) == NULL) {
		syslog(LOG_ERR, "ERROR: calloc");
		exit(1);
	}
	if (ipv6) {
		/* Zero data structures */
		memset(&server_addr6, 0, sizeof(struct sockaddr_in6));
		memset(&imreq, 0, sizeof(struct ipv6_mreq));
		/* Open the UDP socket */
		if ((sock = socket(AF_INET6, SOCK_DGRAM, 0)) == -1) {
			syslog(LOG_ERR, "ERROR: socket");
			exit(1);
		}
		/* Set up the socket */
		server_addr6.sin6_family = AF_INET6;
		server_addr6.sin6_port = htons(port);
		server_addr6.sin6_addr = in6addr_any;
		/* Bind socket */
		if (bind(sock, (struct sockaddr *)&server_addr6,
					sizeof(struct sockaddr_in6)) == -1) {
			syslog(LOG_ERR, "ERROR: bind");
			exit(1);
		}
		inet_pton(AF_INET6, "ff02::1", &imreq.ipv6mr_multiaddr);
		/* Configure the socket for broadcast */
		if (setsockopt(sock, 0, IPV6_ADD_MEMBERSHIP, (const void *)&imreq,
					sizeof(struct ipv6_mreq)) == -1) {
			syslog(LOG_ERR, "ERROR: setsockopt (IPV6_ADD_MEMBERSHIP)");
		}
	} else {
		/* Open the UDP socket */
		if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
			syslog(LOG_ERR, "ERROR: socket");
			exit(1);
		}
		/* Set up and bind the socket */
		server_addr.sin_family = AF_INET;
		server_addr.sin_port = htons(port);
		server_addr.sin_addr.s_addr = INADDR_ANY;
		bzero(&(server_addr.sin_zero), 8);
		if (bind(sock,(struct sockaddr *)&server_addr,
					sizeof(struct sockaddr)) == -1) {
			syslog(LOG_ERR, "ERROR: bind");
			exit(1);
		}
	}
	/* Make process into a daemon */
	daemonize();
	while (1) {
		/* Receive data over our UDP socket */
		bytes_read = recvfrom(sock, data, size, 0,
				(struct sockaddr *)&client_addr, &addr_len);
		/* Logging/debug message */
		syslog(LOG_INFO, "UDP Server received and hashed data from [%s:%d]\n",
				bytes_read,
				inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
		fflush(stdout);
		/* Mix incoming entropy + salt into pool */
		salt = anerd_salt(salt);
		fwrite(&salt, sizeof(uint64_t), 1, fp);
		fwrite(data, bytes_read, 1, fp);
		fflush(fp);
		/* Obtain some entropy for transmission */
		if (fread(data, bytes_read, sizeof(char), fp) > 0) {
			/* Return the favor, sending entropy back to the initiator */
			sendto(sock, data, bytes_read, 0, (struct sockaddr *)&client_addr,
					sizeof(struct sockaddr));
			syslog(LOG_INFO,
					"UDP Server sent hashed entropy to [%s:%d]\n",
					bytes_read, inet_ntoa(client_addr.sin_addr),
					ntohs(client_addr.sin_port));
		} else {
			syslog(LOG_ERR, "ERROR: fread");
		}
	}
	/* Should never get here; clean up if we do */
	free(data);
	close(sock);
	fclose(fp);
	return 0;
}

void daemonize() {
	/* Drop user priv if run as root */
	if ((getuid() == 0) || (geteuid == 0)) {
		struct passwd *pw = getpwnam(DAEMON_USER);
		if (pw) {
			syslog(LOG_NOTICE, "Setting User To: " DAEMON_USER);
			setuid(pw->pw_uid);
		}
	}
	/* Change current directory to / so current directory is not locked */
	if ((chdir("/")) < 0) {
		syslog(LOG_ERR, "ERROR: chdir() failed");
		exit(1);
	}
	/* Change file mode mask */
	umask(0);
	/* Run setsid() so that daemon is no longer child of spawning process */
	if (setsid() < 0) {
		syslog(LOG_ERR, "ERROR: setsid() failed");
		exit(1);
	}
	/* Close stdin, stdout, stderr */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
}

int main(int argc, char *argv[]) {
	int arg;
	int size = DEFAULT_EXCHANGE_SIZE;
	int port = DEFAULT_PORT;
	char *device = DEFAULT_DEVICE;
	int ipv6 = 0;
	/* Getopt command-line argument handling */
	while ((arg = getopt(argc, argv, "d:p:s:6")) != -1) {
		switch (arg) {
			case '6':
				ipv6 = 1;
				break;
			case 'd':
				device = optarg;
				break;
			case 'p':
				if (isdigit(optarg[0])) {
					port = atoi(optarg);
				} else {
					fprintf(stderr, "Option -p requires a integer argument.\n");
					return 1;
				}
				break;
			case 's':
				if (isdigit(optarg[0])) {
					size = atoi(optarg);
				} else {
					fprintf(stderr, "Option -s requires a integer argument.\n");
					return 1;
				}
				break;
			case '?':
				if (
						(optopt == 'd') || (optopt == 'i') ||
						(optopt == 'p') || (optopt == 's')) {
					fprintf(stderr, "Option -%c requires an argument.\n", optopt);
				} else {
					fprintf(stderr, "Unknown option -%c.\n", optopt);
				}
				return 1;
			default:
				abort();
		}
	}
	/* Set up syslog */
	openlog ("anerd", LOG_PERROR | LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);
	/* fork off process */
	int pid = fork();
	if (pid == 0) {
		/* if process is child */
		/* Fork a server process */
		anerd_server(device, size, port, ipv6);
	} else if (pid > 0) {
		/* if process is parent */
		/* exit, freeing terminal that called program */
		exit(0);
	} else if (pid < 0) {
		/* if the fork() call failed */
		/* exit because of error */
		exit(1);
	}
}
