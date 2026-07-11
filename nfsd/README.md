# gnfsd — 간이 NFSv2 서버

현재(Linux) 머신의 폴더 하나를 NFS로 공유해, 레트로 클라이언트
(NeXTSTEP·BeOS·SunOS 세대 등)가 mount 하고 파일 입출력을 하도록
한다. NFS 스펙(ONC RPC/XDR + MOUNT + NFS)을 **직접 구현**한 것이며
Linux 커널 NFS를 쓰지 않는다.

GrandCrossDevServer의 공유 스토리지 계층(README 전제 3) — 원격
머신이 이 폴더를 작업 디렉토리로 mount 하고, `gcds`로 그 위에서
컴파일을 지시하는 구성을 위한 companion 도구.

## 구현 범위

- **NFSv2 over UDP** (RFC 1094) — 레트로 클라이언트 공통 최저 버전.
- portmap `GETPORT`(RFC 1057) + MOUNT v1(`MNT`/`UMNT`/`EXPORT`) +
  NFS v2 프로시저: NULL, GETATTR, SETATTR, LOOKUP, READ, WRITE,
  CREATE, REMOVE, RENAME, MKDIR, RMDIR, READDIR, STATFS.
- **두 UDP 포트**: portmapper(기본 111)와 mount+nfs(기본 2049).
  두 소켓 모두 세 프로그램을 처리한다. 포트를 나눈 이유는
  **NeXTSTEP/OPENSTEP 같은 구형 클라이언트가 NFS 요청을 GETPORT
  결과와 무관하게 2049로 보내기** 때문(§실전 참고). 단일 스레드,
  select 다중 소켓.
- **중복요청 캐시(DRC)**: UDP 재전송으로 비멱등 연산(CREATE/REMOVE/
  RENAME/MKDIR/RMDIR/WRITE/SETATTR)이 두 번 실행되는 것을 방지 —
  최근 `(클라이언트 endpoint, xid) → 응답`을 캐시(256슬롯/30초)해
  재전송이면 그대로 재생. 멱등 연산(READ/GETATTR 등)은 캐시 안 함.
- **sub-second mtime**: fattr에 usec까지 보고해 같은-초 파일 변경도
  클라이언트가 감지.
- 파일 핸들 32바이트 = 경로 테이블 인덱스 + 매직 + 검증자.
- **권한: 단일 uid로 정규화(squash)**. 특권 포트(111) 바인딩 후에는
  root가 불필요하므로, 바인딩 직후 **일반 uid/gid로 영구히 권한을
  떨어뜨린다**(기본: export 디렉토리 소유자, `-u`로 지정). 그래서
  어떤 클라이언트가 쓰든 파일은 그 한 사용자 소유로 생성되고
  (root 소유 파일 난립 없음), 서버는 그 사용자 권한 밖을 건드리지
  못한다(보안 이득). 클라이언트별 uid 매핑 같은 세세한 권한 제어는
  하지 않는다(설계상).

## 빌드 / 실행

```
make                                  # ./gnfsd
./gnfsd [-p PMAP] [-n NFS] [-v] <공유디렉토리>
#   -p PMAP  portmapper 포트 (기본 111)
#   -n NFS   mount+nfs 포트 (기본 2049)
#   -v       요청별 로그
```

편의 스크립트(빌드 자동 + 포트/권한 처리 + mount 안내):
```
./serve.sh <공유디렉토리>              # 포트 111 (root 필요)
sudo ./serve.sh /srv/share            # 실배포
./serve.sh -p 12049 /tmp/share        # 고포트, 개발용(백그라운드)
./serve.sh -f -p 12049 /tmp/share     # 포그라운드(-f), Ctrl-C로 정지
```

- 레트로 클라이언트는 portmapper를 **포트 111**(특권 <1024)에서
  찾으므로 실배포는 **root로 실행**해야 한다.
- serve.sh는 root 아닌데 111을 고르면 경고하고 고포트 예시를 안내.

## 테스트 (root/mount 불필요)

```
make test
```
`test-nfs.py`가 UDP로 RPC를 직접 던져(mount 없이) portmap→MOUNT→
GETATTR/CREATE/WRITE/READ/LOOKUP/MKDIR/READDIR/STATFS/REMOVE/RMDIR을
검증한다. CDSP를 raw 클라이언트로 검증한 방식과 동일.

## 실제 mount (OPENSTEP)

root로 111+2049에 띄운 뒤 NeXTSTEP/OPENSTEP 클라이언트에서:
```
mount -t nfs -o soft,timeo=10 <서버IP>:/공유경로 /nfstest
```

### 소스가 자주 바뀌면 `-o noac` (중요)

빌드용 소스 공유처럼 파일이 계속 바뀌고 **즉시 반영**이 필요하면
클라이언트의 속성 캐시를 끈다:
```
mount -t nfs -o soft,timeo=10,noac <서버IP>:/공유경로 /nfstest
```
NFSv2 클라이언트는 mtime 기반 속성 캐시를 두어(수 초~수십 초),
캐시 유효 동안 갱신을 못 본다(remount 필요). `noac`(또는 낮은
`actimeo`)면 매번 서버에 재확인해 갱신이 바로 보인다. 트레이드오프는
GETATTR 트래픽 증가뿐. gnfsd는 sub-second mtime을 보고하므로,
`noac`가 아니어도 캐시 만료 후엔 같은-초 변경까지 감지한다.
실기 OPENSTEP(NeXT Mach)에서 전 항목 통과:
- mount 성공(RC=0), 서버 파일 목록·읽기(`ls`/`cat`)
- **쓰기 양방향**: OPENSTEP에서 만든 파일/디렉토리/`cp`가 Linux
  서버에 즉시 반영, 반대도 동일
- 109KB 파일 다중블록(8KB) 읽기 `sum` 체크섬 양쪽 일치
- 깨끗한 umount

### 왜 2049도 여는가

**NeXTSTEP은 NFS 요청을 portmap GETPORT 결과와 무관하게 포트 2049로
보낸다**(구형 클라이언트 관례). portmapper(111)만 열고 서비스를 다른
포트에 두면 MNT는 되지만 실제 NFS I/O가 2049로 가 무응답이 되므로,
mount+nfs를 2049에 함께 띄운다. (uid 매핑이 없어 서버 프로세스 uid로
파일 생성 — 세세한 권한 제어 없음, 설계대로.)

## 한계 (v1)

- NFSv2/UDP만. NFSv3·TCP·NLM(파일 잠금)은 미구현.
- 32비트 크기 필드 → 4GB 이상 파일 미표현(NFSv2 스펙 한계).
- 단일 export 디렉토리, uid 매핑/exports 접근제어 없음.
- READDIR은 호출당 재스캔(O(n^2)) — 소규모 디렉토리 전제.
