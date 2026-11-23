// [adder.c] CGI 예제 — 두 수를 더한 결과를 출력 (동적 요청 처리 데모)
// 서버가 stdout을 클라이언트 소켓에 연결(dup2)해 두므로 printf가 곧 HTTP 응답이 된다
#include <stdio.h>

int main(void) {
    // TODO: GET이면 QUERY_STRING("a=1&b=2"), POST면 stdin에서 CONTENT_LENGTH만큼 읽어 두 수 추출
    // TODO: 헤더(Content-Type, Content-Length) + 빈 줄 + 본문 출력
    printf("Content-Type: text/plain\r\n\r\nTODO\n");
    return 0;
}
