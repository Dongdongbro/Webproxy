// Sequential caching web proxy
// 하나의 클라이언트만 연결하는 형태.

// 프록시를 왜 쓰는가? 부하를 덜어준다. 그리고 실행 중에 클라이언트 쪽 fd와 서버 쪽 fd를 켜둔 상태로 있으므로 보다 더 빠르게 전송가능

#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestlint_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";    // 요청의 끝부분을 의미.

static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_endServer(char *hostname, int port, char *http_header);

int main(int argc, char **argv) {
  int listenfd, connfd;
  socklen_t clientlen;
  char hostname[MAXLINE], port[MAXLINE];

  struct sockaddr_storage clientaddr; /* generic sockaddr struct which is 28 Bytes. The same use as sockaddr */

  if (argc != 2) {
    fprintf(stderr, "usage :%s <port> \n", argv[0]);
    exit(1);
  }

  // Open_listenfd 함수를 호출해서 듣기 소켓 오픈. 인자로 포트 번호 넘겨줌
  // listenfd에 듣기 식별자 리턴
  // 프록시가 서버가 하는 것처럼 듣기 소켓을 만들기
  listenfd = Open_listenfd(argv[1]);
  while (1) {
    clientlen = sizeof(clientaddr);   // 클라이언트 주소 길이
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);   // listenfd와 clientaddr를 합쳐서 connfd 만들기. 연결 요청 접수

    /* print accepted message */
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0); // 소켓 구조체를 호스트의 서비스들(문자열)로 변환
    printf("Accepted connection from (%s %s).\n", hostname, port);

    /* sequential handle the client transaction */
    doit(connfd);   // 트랜잭션 수행
    Close(connfd);  // 연결 끝. 소켓 닫기.
  }
  // printf("%s", user_agent_hdr);
  return 0;
}

/* handle the client HTTP transaction */
void doit(int connfd) {
  int end_serverfd; /* the end server file desciptor */

  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char endserver_http_header[MAXLINE];
  /* store the request line arguments */
  char hostname[MAXLINE], path[MAXLINE];
  int port;

// rio의 장점? EOF를 만났을 때 short count가 나옴을 보장함. short count : 원하는 만큼 읽겠다고 했는데 다 못 읽는 경우에 발생.
  rio_t rio, server_rio;  /* rio is client's rio, server_rio is endserver's rio */
  Rio_readinitb(&rio, connfd);    // 읽기 버퍼 초기화. rio_t 타입의 읽기 버퍼와 식별자 connfd 연결
  Rio_readlineb(&rio, buf, MAXLINE);  // 클라이언트가 보낸 요청 라인을 읽고 분석. buf에 복사
  sscanf(buf, "%s %s %s", method, uri, version);  /* read the client request line */  // 문자열에서 형식화된 데이터 읽어와서 각 변수에 맵핑

  if (strcasecmp(method, "GET")) {    // GET이 아니면 처리하지 않음
    printf("Proxy does not implement the method");
    return;
  }
  // read_requesthdrs가 없다.

  /* pause the uri to get hostname, file path, port */
  // uri를 파싱해서 hostname, path, port번호에 값 맵핑
  parse_uri(uri, hostname, path, &port);

  /* build the http header which will send to the end server */
  // 엔드 서버로 보낼 http 헤더를 빌드합니다.
  build_http_header(endserver_http_header, hostname, path, port, &rio); // 헤더, 호스트 이름, 경로, 포트, 읽기 버퍼

  /* connect to the end server */
  // 엔드 서버에 연결하면서 입출력에 대해 준비된 소켓 식별자 리턴
  // 서버 입장에서는 프록시가 클라이언트
  end_serverfd = connect_endServer(hostname, port, endserver_http_header);
  if (end_serverfd < 0) {
    printf("connection failed\n");
    return;
  }

  Rio_readinitb(&server_rio, end_serverfd); // 서버의 읽기 버퍼 초기화. rio_t 타입의 읽기 버퍼와 식별자 fd 연결
  /* write the http header to endserver */
  Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header)); // 아까 만든 http 헤더를 fd에 write

  /* receive message from end server and send to client */
  // 최종 서버로부터 메시지 수신 후 클라이언트에게 전송
  size_t n;   // 버퍼에다 쓴 내용의 바이트 크기
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {   // server_rio에 있는 내용들을 모두 한줄씩 읽으면서 buf에 복사하고 connfd에 write
    printf("proxy received %ld bytes, then send\n", n);
    Rio_writen(connfd, buf, n);   // 서버에게서 받은 메시지를 클라이언트에게 줄 connfd에 write
  }
  Close(end_serverfd);    // 식별자 다 썼으니 Close
}

// 헤더, 호스트 이름, 경로, 포트, 읽기 버퍼(현재 클라이언트의 요청이 들어와있음)
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio) {
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
  /* request lint */
  sprintf(request_hdr, requestlint_hdr_format, path); // request_hdr에 "GET %s HTTP/1.0\r\n" 이 폼으로 경로 넣어서 저장
  //path = /index.html
  /* get other request header for client rio and change it */
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {   // 읽기에 성공한 경우  client_rio : 아까 가져온 클라이언트의 읽기 버퍼. buf에 옮겨쓰기
    if (strcmp(buf, endof_hdr) == 0)  /* EOF */   // 종료 조건
      break;

    if (!strncasecmp(buf, host_key, strlen(host_key))) {  // buf의 내용이 "Host"이면 true
      strcpy(host_hdr, buf);  // host_hdr에 입력
      continue;
    }

    if (strncasecmp(buf, connection_key, strlen(connection_key))   // buf의 내용이 "Connection"이면 true
          && strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))  // "Proxy-Connection"
          && strncasecmp(buf, user_agent_key, strlen(user_agent_key))) {   // "User-Agent"
      strcat(other_hdr, buf);   // buf를 남은 헤더에 연결
    }
  }

  if (strlen(host_hdr) == 0) {    // 호스트 헤더가 비었다면
    sprintf(host_hdr, host_hdr_format, hostname);   // 호스트 이름을 "Host: %s\r\n" 폼에 넣고 Host_hdr에 출력
  }
  // http_header에 요청 헤더, 호스트 헤더, 연결 헤더, 프록시 헤더.... 들을 출력
  sprintf(http_header, "%s%s%s%s%s%s%s", request_hdr, host_hdr, conn_hdr, prox_hdr, user_agent_hdr, other_hdr, endof_hdr);

  return;
}

/* Connect to the end server */
// 엔드 서버와 연결. 여기서 엔드 서버라는 것은 tiny 서버를 말하는 것이겠지?
// inline 함수 : 호출하는 함수가 아닌 컴파일 시 호출부 위치에 함수 코드 자체를 복사. 호출 과정이 없어서 속도가 빠르지만 많이 사용하면 실행 코드의 크기가 커질 수 있다
inline int connect_endServer(char *hostname, int port, char *http_header) {
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);  // 클라이언트 입장에서 fd 열기. 서버와 연결 설정
}

/* parse the uri to get hostname, file path, port */
void parse_uri(char *uri, char *hostname, char *path, int *port) {
  *port = 8080; // 디플트 값.
  char *pos = strstr(uri, "//");    // 문자열에 담긴 '//'을 찾고 그 위치를 반환. 못 찾으면 NULL. http'//' 인 경우에 무시하려고 넣은 코드인듯

  pos = pos != NULL ? pos+2 : uri;  // pos안에 '//'가 있으면 포인터를 슬래시 다음으로, 없으면 pos = uri

  char *pos2 = strstr(pos, ":");  // ':' 찾고 포인터 반환 (포트를 받기위해서)
  if (pos2 != NULL) {   // ':'가 있다면 ex) localhost:8000/home.html
    *pos2 = '\0';   // 이게 무슨 뜻이지? : 자리에 \0이 들어온다. 스트링을 끊어주는 역할. localhost\08000/home.html
    sscanf(pos, "%s", hostname);  //pos = www.cmu~~ <-hostname에다가 넣는다. // localhost
    sscanf(pos2+1, "%d%s", port, path); // port : 8000, path = /home.html
  } else {    // ':'가 없다면 무슨 경우? 포트를 입력하지 않았을 때. 디폴트 포트 8080 사용
    pos2 = strstr(pos, "/");
    if (pos2 != NULL) {
      *pos2 = '\0';
      sscanf(pos, "%s", hostname);
      *pos2 = '/';
      sscanf(pos2, "%s", path);
    } else {
      sscanf(pos, "%s", hostname);
    }
  }
  return;
}