#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include "includes/aws.h"
#include "includes/util.h"
#include "includes/debug.h"
#include "includes/sock_util.h"
#include "includes/w_epoll.h"
#include "includes/http_parser.h"
#include <sys/sendfile.h>
#include <fcntl.h>	/* O_CREAT, O_RDONLY */

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	enum connection_state state;
};

char buffer[BUFSIZ] =
		"HTTP/1.1 200 OK\r\n"
				"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
				"Server: Apache/2.2.9\r\n"
				"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
				"Accept-Ranges: bytes\r\n"
				"Content-Length: 153\r\n"
				"Vary: Accept-Encoding\r\n"
				"Connection: close\r\n"
				"Content-Type: text/html\r\n"
				"\r\n"
				"<html><head><meta name=\"google-site-verification\" content=\"gTsIxyV43HSJraRPl6X1A5jzGFgQ3N__hKAcuL2QsO8\" /></head>"
				"<body><h1>It works!</h1></body></html>\r\n";

const char err_404[BUFSIZ] =
		"HTTP/1.1 404 Not Found\r\n"
				"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
				"Server: Apache/2.2.9\r\n"
				"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
				"Accept-Ranges: bytes\r\n"
				"Content-Length: 153\r\n"
				"Vary: Accept-Encoding\r\n"
				"Connection: close\r\n"
				"Content-Type: text/html\r\n"
				"\r\n"
				"<html><head><meta name=\"google-site-verification\" content=\"gTsIxyV43HSJraRPl6X1A5jzGFgQ3N__hKAcuL2QsO8\" /></head>"
				"<body><h1>Error 404: Page not found</h1></body></html>\r\n";
/*
const char err_404[] ="HTTP/1.1 404 Not Found"
"Connection: close"
"Content-Type: text/html"

"<html>"
  "<body>"
      "Error 404: Page not Found"
  "</body>"
"</html>";
*/
static http_parser request_parser;
static char request_path[BUFSIZ];	/* storage for request_path */


/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, buf, len);

	return 0;
}

static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};


/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));
	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);

	return conn;
}

/*
 * Copy receive buffer to send buffer (echo).
 */
/*
static void connection_copy_buffers(struct connection *conn)
{
	conn->send_len = conn->recv_len;
	memcpy(conn->send_buffer, conn->recv_buffer, conn->send_len);
}
*/
/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_ERR, "Accepted connection from: %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_recv = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	printf("--\n%s--\n", conn->recv_buffer);

	conn->recv_len = bytes_recv;
	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_sent = send(conn->sockfd, conn->send_buffer, strlen(conn->send_buffer), 0);
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

	printf("--\n%s--\n", conn->send_buffer);

	/* all done - remove out notification */
	rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");

	conn->state = STATE_DATA_SENT;

	return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn) {
	int rc, fd;
	struct stat stat_buf;
	enum connection_state ret_state;
	size_t bytes_parsed;
	char realpath[BUFSIZ + 1];
	//-6-7
	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;
	http_parser_init(&request_parser, HTTP_REQUEST);

	strcpy(request_path, "");
	bytes_parsed = http_parser_execute(&request_parser, &settings_on_path,
			conn->recv_buffer, strlen(conn->recv_buffer));
	//printf("Parsed complex HTTP request (bytes: %lu), path: %s\n", bytes_parsed,
	//		request_path);
	//connection_copy_buffers(conn);
	if (strncmp(request_path, "/static", 7) != 0
			&& strncmp(request_path, "/dynamic", 8) != 0) {
		strcpy(conn->send_buffer, err_404);
	} else {
		if (strncmp(request_path, "/static", 7) == 0) {
			//printf("Branched STATIC FOLDER\n");
			strcpy(realpath, ".");
			strcat(realpath, request_path);
			fd = open(realpath, O_RDONLY);
			fstat(fd, &stat_buf);
			if (fd < 0) {
				//printf("Failed to open folder\n");
				strcpy(conn->send_buffer, err_404);
				rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
				return;
			} else {
				//printf("Sending file\n");
				sendfile(conn->sockfd, fd, NULL, stat_buf.st_size);
				printf("Sent file\n");
				close(fd);
				rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
				return;
			}
		} else if (strncmp(request_path, "/dnyamic", 8) == 0) {
			strcpy(realpath, ".");
			strcat(realpath, request_path);
		}
		strcpy(conn->send_buffer, buffer);
		return;
	}
	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_inout");
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);


	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		}
		else {
			if (rev.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New message\n");
				handle_client_request(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				send_message(rev.data.ptr);
			}
		}
	}

	return 0;
}
