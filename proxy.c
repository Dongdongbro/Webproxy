#include <stdio.h>
#include "csapp.h"

void cache_init();
void *thread_routine(void *connfdp);
void doit(int connfd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *HTTPheader, char *hostname, char *path, int port, rio_t *client_rio);

int main(int argc, char **argv)
{
    int listenfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    // 캐시 ON
    cache_init();
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(clientaddr);
        int *connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid, NULL, thread_routine, connfdp);
    }
    return 0;
}

void *thread_routine(void *connfdp)
{
    int connfd = *((int *)connfdp);
    Pthread_detach(pthread_self());
    Free(connfdp);
    doit(connfd);
    Close(connfd);
}


#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_OBJECT_NUM ((int)(MAX_CACHE_SIZE / MAX_OBJECT_SIZE))

typedef struct
{
    char cache_obj[MAX_OBJECT_SIZE]; // 헤더정보와 같은 캐시컨텐츠를 담기위한 공간
    char cache_uri[MAXLINE];         // uri정보를 담는 곳
    int order;                       // LRU order
    int alloc, read;                 // alloc 캐시가 할당이 되어있는지 되어있는지, read 캐시를 읽고 있는 애들
    sem_t w, rs;                     // write와 read 세마포어를 설정해준다.
} cache_block;

typedef struct
{
    cache_block cacheOBJ[MAX_OBJECT_NUM];
} Cache;

// cache 스트럭쳐의 초기값을 설정
Cache cache;
void cache_init()
{
    int index = 0;
    for (; index < MAX_OBJECT_NUM; index++)
    {
        cache.cacheOBJ[index].order = 0;
        cache.cacheOBJ[index].alloc = 0;
        Sem_init(&cache.cacheOBJ[index].w, 0, 1);       // 세마포어를 처음에 1로 설정해주어 flag를 1로 만들어준다. 중간에 0값은 thread를 받아줄것이라는 명시적 약속이다.
        Sem_init(&cache.cacheOBJ[index].rs, 0, 1);
        cache.cacheOBJ[index].read = 0;                 // read 캐시를 읽고있는 애들을 0으로 설정해준다.
    }
}

// 캐시를 읽기 전 세마포어 연산으로 타 스레드로부터 보호
void readstart(int index)
{
    P(&cache.cacheOBJ[index].rs);       // read + 1 하고 첫번째 들어온 스레드에 대해서 처리 해주는 동안에는 다른 스레드가 접근하는 것을 막아주기 위해서 wait처리를 해준다.
    cache.cacheOBJ[index].read = cache.cacheOBJ[index].read + 1;  // 캐시를 읽고 있는 스레드 갯수에 +1
    if (cache.cacheOBJ[index].read == 1)               // 읽고 있는 스레드가 하나라도 있을 때는 write를 하지 못하게 하기 위해서
        P(&cache.cacheOBJ[index].w);
    V(&cache.cacheOBJ[index].rs);       // write 만 막아주고 read에 대한 접근 권한은 열어둔다.
}

// readstart의 역연산으로 돌려놓음
void readend(int index)
{
    P(&cache.cacheOBJ[index].rs);       // read - 1 하고 마지막에 나가는 스레드를 처리하는 과정 중에는 접근할 수 없도록 제한
    cache.cacheOBJ[index].read = cache.cacheOBJ[index].read - 1;
    if (cache.cacheOBJ[index].read == 0)              // 읽고 있는 스레드가 하나도 없을 때는 캐시에 write를 할 수 있도록 처리
        V(&cache.cacheOBJ[index].w);
    V(&cache.cacheOBJ[index].rs);
}

// 가용한 캐시가 있는지 탐색하고 있다면 index를 리턴하는 함수
int cache_find(char *uri)
{
    int index = 0;
    // 각 캐시에 대해 탐색
    for (; index < MAX_OBJECT_NUM; index++)
    {
        // 탐색 전 세마포어 보호
        readstart(index);
        // 캐시적중한다면 해당 인덱스에 정보가 있다는 것임으로 더 이상 for문을 돌리지 않고, 아니라면 계속 탐색
        if (cache.cacheOBJ[index].alloc && (strcmp(uri, cache.cacheOBJ[index].cache_uri) == 0))
            break;
        readend(index);
    }
    // 가용한 캐시가 없다면 -1을 return
    if (index == MAX_OBJECT_NUM)
        return -1;
    return index;
}

// 빈 캐시, 혹은 LRU order가 제일 낮은 캐시를 골라 index를 리턴하는 함수
int cache_eviction()
{
    //minorder는 upper bound에서 시작해서 자신보다 낮은 값이 나올때마다 갱신된다
    int minorder = MAX_OBJECT_NUM + 1;
    int minindex = 0;
    int index = 0;
    // 모든 index를 탐색하며 비교
    for (; index < MAX_OBJECT_NUM; index++)
    {
        readstart(index);
        // 캐시가 비어있다면, 해당 인덱스 값을 리턴해준다.
        if (!cache.cacheOBJ[index].alloc)
        {
            readend(index);
            return index;
        }
        // 빈 캐시가 발견되지 않는 동안 minorder를 갱신하며 탐색
        if (cache.cacheOBJ[index].order < minorder)
        {
            minindex = index;       // minindex를 매번 갱신해주며 더 작은 값을 찾도록 한다.
            minorder = cache.cacheOBJ[index].order;     // minorder 에 현재 인덱스를 넣어줌으로써, 비교 대상을 더 작은 인덱스로 갱신
        }
    readend(index);
    }
    // 빈 캐시가 발견되지 않고 for문이 종료되었다면 minindex를 return
    return minindex;
}

// 최근 사용된 캐시의 LRU order를 재정렬하는 함수
void cache_reorder(int target)
{
    // 방금 쓴 target을 최고 order로
    cache.cacheOBJ[target].order = MAX_OBJECT_NUM + 1;
    int index = 0;
    for (; index < MAX_OBJECT_NUM; index++)
    {
        // 나머지는 모두 order값을 1씩 낮춰주어 가장 최근 사용된 캐시의 값을 가장 높을 수 있도록 한다.
        if (index != target)     // 내가 아닌경우
        {
            P(&cache.cacheOBJ[index].w);
            cache.cacheOBJ[index].order = cache.cacheOBJ[index].order - 1;
            V(&cache.cacheOBJ[index].w);
        }
    }
}

// cache_eviction으로 차출된 캐시에 uri와 buf를기록하는 함수
void cache_uri(char *uri, char *buf)
{
    // 차출
    int index = cache_eviction();
    // 쓰기 전 세마포어 보호
    P(&cache.cacheOBJ[index].w);
    // buf, uri 카피
    strcpy(cache.cacheOBJ[index].cache_obj, buf);
    strcpy(cache.cacheOBJ[index].cache_uri, uri);
    // 사용 캐시 인덱스 alloc를 정보가 들어있는 캐시로 해준다.
    cache.cacheOBJ[index].alloc = 1;
    // LRU order 재정렬
    cache_reorder(index);
    // 보호 해제
    V(&cache.cacheOBJ[index].w);
}

void doit(int connfd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char HTTPheader[MAXLINE], hostname[MAXLINE], path[MAXLINE];
    int backfd;
    rio_t rio, backrio;

    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement this method\n");
        return;
    }

    // uri를 MAX_OBJECT_SIZE가 허용하는 만큼만 uri_store에 담는다
    char uri_store[MAX_OBJECT_SIZE];
    strcpy(uri_store, uri);
    int cache_index;
    // 캐시에 있는지 확인
    if ((cache_index = cache_find(uri_store)) != -1) // 캐시 적중했을시
    {
        // 있다면 캐시에서 보내고 doit 종료
        readstart(cache_index);
        Rio_writen(connfd, cache.cacheOBJ[cache_index].cache_obj, strlen(cache.cacheOBJ[cache_index].cache_obj));
        readend(cache_index);
        return;
    }
    int port;
    // uri를 파싱하는 목적은 서버마다 다른데, 프록시 서버에서의 목적은 hostname과 path를 추출하고 포트를 결정하는 것이다
    // 아래에서 이 목적에 따르는 코드로 parse_uri를 구현
    parse_uri(uri, hostname, path, &port);
    // 결정된 hostname, path, port에 따라 HTTP header를 만든다
    makeHTTPheader(HTTPheader, hostname, path, port, &rio);

    char portch[10];
    sprintf(portch, "%d", port);
    // back과 연결 후 만든 HTTP header를 보낸다
    backfd = Open_clientfd(hostname, portch);
    if(backfd < 0)
    {
        printf("connection failed\n");
        return;
    }
    Rio_readinitb(&backrio, backfd);
    Rio_writen(backfd, HTTPheader, strlen(HTTPheader));
    //캐시로 보낼 정보를 임시로 담는 공간
    char cachebuf[MAX_OBJECT_SIZE];
    size_t sizerecvd, sizebuf = 0;      //sizerecvd는 서버의 버퍼를 한줄 한줄 가져오는 문자열역할
    while((sizerecvd = Rio_readlineb(&backrio, buf, MAXLINE)) != 0)
    {
        // 이때 buf의 크기가 MAX_OBJECT_SIZE보다 작다면 cachebuf에 카피하고
        sizebuf = sizebuf + sizerecvd;
        if (sizebuf < MAX_OBJECT_SIZE)
        {
            strcat(cachebuf, buf);
        }
        printf("proxy received %d bytes, then send\n", sizerecvd);
        Rio_writen(connfd, buf, sizerecvd);
    }
    Close(backfd);
    if (sizebuf < MAX_OBJECT_SIZE)
    {
        // cachebuf를 cache에 기록한다
        cache_uri(uri_store, cachebuf);
    }
}

int parse_uri(char *uri, char *hostname, char *path, int *port)
{
    *port = 80;
    char *hostnameP = strstr(uri, "//");
    if (hostnameP != NULL)
    {
        hostnameP = hostnameP + 2;
    }
    else
    {
        hostnameP = uri;
    }
    char *pathP = strstr(hostnameP, ":");
    if(pathP != NULL)
    {
        *pathP = '\0';
        sscanf(hostnameP, "%s", hostname);
        sscanf(pathP + 1, "%d%s", port, path);
    }
    else
    {
        pathP = strstr(hostnameP, "/");
        if(pathP != NULL)
        {
            *pathP = '\0';
            sscanf(hostnameP, "%s", hostname);
            *pathP = '/';
            sscanf(pathP, "%s", path);
        }
        else
        {
            sscanf(hostnameP, "%s", hostname);
        }
    }
    return 0;
}

static const char *user_agent_header = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_header = "Connection: close\r\n";
static const char *prox_header = "Proxy-Connection: close\r\n";
static const char *host_header_format = "Host: %s\r\n";
static const char *requestlint_header_format = "GET %s HTTP/1.0\r\n";
static const char *endof_header = "\r\n";
static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";
void makeHTTPheader(char *HTTPheader, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_header[MAXLINE], other_header[MAXLINE], host_header[MAXLINE];
    sprintf(request_header, requestlint_header_format, path);
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        if(strcmp(buf, endof_header) == 0)
        {
            break;
        }
        if(!strncasecmp(buf, host_key, strlen(host_key)))
        {
            strcpy(host_header, buf);
            continue;
        }
        if(!strncasecmp(buf, connection_key, strlen(connection_key))
                &&!strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
                &&!strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            strcat(other_header, buf);
        }
    }
    if(strlen(host_header) == 0)
    {
        sprintf(host_header, host_header_format, hostname);
    }
    sprintf(HTTPheader, "%s%s%s%s%s%s%s", request_header, host_header, conn_header, prox_header, user_agent_header, other_header, endof_header);
}