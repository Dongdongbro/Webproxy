/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
char version[MAXLINE]; // 11.6 C문제 풀이 version 전역변수 설정

// MAXLINE, MAXBUF == 8192

// 인자 값 ./tiny 8000, argc = 2, argv[0] = tiny, argv[1] = 8000
int main(int argc, char **argv){
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  // Open_listenfd를 호출하여 듣기 소켓을 오픈해준다.
  listenfd = Open_listenfd(argv[1]);
  // 무한 서버 루프 실행
  while (1) {
    clientlen = sizeof(clientaddr);     //accept에 길이를 넣어줄 인자를 생성
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);  // Accept(듣기 식별자, 소켓구조체 주소, 주소의 길이)를 받아 듣기 식별자를 받아 connect를 위한 새로운 식별자를 리턴한다.
    // addrinfo구조체에 담겨있는 정보를 스트링표시로 전환하여 버퍼에 저장한다.
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    // 트랜젝션 수행
    // echo(connfd);
    doit(connfd);   // line:netp:tiny:doit
    // 트랜젝션이 수행된 후 자신 쪽의 연결 끝(소켓)을 닫는다
    Close(connfd);  // line:netp:tiny:close
  }
}

void echo(int connfd){
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while((n = Rio_readlineb(&rio,buf,MAXLINE))!=0){
        printf("%s",buf);
        Rio_writen(connfd,buf,n);
    }
}

void doit(int fd){
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  // rio(Robust I/O)
  rio_t rio;
  //rio_t 구조체의 정보들
  //   typedef struct {
  //     int rio_fd;                /* 내부 버퍼의 식별자 저장 */
  //     int rio_cnt;               /* 내부버퍼 읽어질 바이트 수 저장 */
  //     char *rio_bufptr;          /* 다음 읽을 1바이트 크기의 문자의 주솟값 */
  //     char rio_buf[RIO_BUFSIZE]; /* 내부 버퍼[크기] */
  // } rio_t;

  // 비어있는 rio 구조체에 connd 식별자를 넣어준다.
  Rio_readinitb(&rio, fd);
  // 요청 라인을 읽어들이고 분석, return값은 버퍼 크기의 n-1을 리턴해준다.
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request header:\n");
  printf("%s",buf);
  sscanf(buf, "%s %s %s",method, uri, version); // 11.6 C문제 풀이

  // Tiny는 GET 만 지원하기에 method에 입력된 글자와 GET을 비교해서 다르면 return을 해준다.
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")){ // 11.11 문제로 HEAD 추가
    clienterror(fd, method, "501", "Not implemented","Tiny does not implement this method");
    return;
  }

  // GET이 들어왔다면 읽어들이고, 다른 요청 헤더들은 무시한다.
  read_requesthdrs(&rio);

  /* URI 를 파일 이름과 비어 있을 수도 있는 CGI 인자 스트링으로 분석하고, 요청이 정적 또는 동적 컨텐츠를 위한 것인지 나타내는 플래그를 설정한다.  */
  is_static = parse_uri(uri, filename, cgiargs);
  // GET으로 실행하려는 파일이 디스크에 저장되어있지 않으면, 에러메시지를 보내고, 메인으로 리턴.
  if (stat(filename, &sbuf) < 0) {
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  // 파일이 정적 콘텐츠일떄
  if (is_static){
    // 파일형식이 '-'로 시작하는 파일이 아니고, 읽기 가능파일이 아니라면 에러메세지 -> 리턴
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    // 파일형식이 '-r------' 형식이라면 정적콘텐츠로 제공
    serve_static(fd, filename, sbuf.st_size);
  }
  // 파일이 동적 콘텐츠일때,
  else {
    // 파일형식이 '-'로 시작하는 일반파일이 아니고, 실행가능한 파일이 아니라면 에러메세지 -> 리턴
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI Program");
      return;
    }
    // 파일형식이 '-rwx-----' 형식이라면 동적콘턴츠로 제공
    serve_dynamic(fd, filename, cgiargs);
  }
}

// 명백한 오류에 대해서 클라이언트에 보고하는 함수
// HTTP응답을 응답 라인에 적절한 상태 코드와 상태 메세지와 함께 클라이언트에 보낸다.
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg){
  char buf[MAXLINE], body[MAXBUF];
  // Build the HTTP response body
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n",body);

  // Print the HTTP response
  sprintf(buf, "%s %s %s\r\n",version, errnum, shortmsg); // 11.6 C문제 풀이
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Contnet-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}

void read_requesthdrs(rio_t *rp){
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  // 헤더의 마지막 줄은 비어있기에 \r\n 만 buf에 담겨있다면 while문을 탈출한다.
  // strcmp 문자열을 비교하는 함수
  while(strcmp(buf, "\r\n")) {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

int parse_uri(char *uri, char *filename, char *cgiargs){
  char *ptr;

  if(!strstr(uri, "cgi-bin")){
    strcpy(cgiargs,"");
    strcpy(filename, ".");
    strcat(filename, uri);
    if (uri[strlen(uri)-1] == '/')
      strcat(filename, "home.html");
    return 1;
  }
  else{
    ptr = index(uri,'?');
    if(ptr){
      strcpy(cgiargs,ptr+1);
      *ptr = '\0';
    }
    else
      strcpy(cgiargs,"");
    strcpy(filename,".");
    strcat(filename, uri);
    return 0;
  }
}


void serve_static(int fd, char *filename, int filesize){
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF], *mbuf;

  // Send response headers to client
  get_filetype(filename, filetype);
  sprintf(buf, "%s 200 OK\r\n", version); // 11.6 C문제 풀이
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContnet-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  // Send response body to clinet
  srcfd = Open(filename, O_RDONLY, 0);

  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  // Close(srcfd);
  // Rio_writen(fd, srcp, filesize);
  // Munmap(srcp, filesize);

  // 11.9 숙제를 위한 malloc 코드
  mbuf = malloc(filesize);
  Rio_readn(srcfd, mbuf, filesize);
  Close(srcfd);
  Rio_writen(fd, mbuf, filesize);
  free(mbuf);

}

// get_filetype - Derive file type from filename
void get_filetype(char *filename, char *filetype){
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs){
  char buf[MAXLINE], *emptylist[] = { NULL };

  // Return first part of HTTP response
  sprintf(buf, "%s 200 OK\r\n",version); // 11.6 C문제 풀이
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  if (Fork() == 0) {
    setenv("QUERY_STRING", cgiargs, 1);
    Dup2(fd, STDOUT_FILENO);
    Execve(filename, emptylist, environ);
  }
  Wait(NULL);
}