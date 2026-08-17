# 수정계획: gnfsd READDIR가 클라이언트 `count`를 무시하는 버그

2026-08-16 인시던트(`.../openstep-sdl20/notes/GNFSD_NFS_REBUILD_INCIDENT_20260816.md`)의
`NFS readdir failed for server 192.168.1.16: RPC: Can't decode result`를
계측으로 규명하고 고치기 위한 구체 계획. 이 문서는 codex 재검토 대상이다.

> 상태: **수정 적용·실기 검증 완료 (2026-08-17).** 원인·측정치 확정(§2,§3),
> 수정 코드 적용(§5), codex 재검토(§10), **실기 OPENSTEP에서 인시던트
> 종단 해소 확인**(§7.2). 남은 것은 커밋뿐.

## 1. 요약 (심각도)

- **버그:** `nfsd/nfs.c`의 `nfs_readdir()`가 READDIR 요청의 `count`
  인자를 **완전히 무시**하고(`(void)count;`) 응답을 하드코딩 7500바이트까지
  채운다. RFC 1094 §2.2.17은 READDIR 응답의 디렉터리 데이터가 `count`
  바이트를 넘지 않아야 한다고 규정한다.
- **영향:** 엔트리가 많은(≈ count 바이트 초과) 디렉터리를 열거하면 서버가
  클라이언트 버퍼보다 큰 응답을 보내 클라이언트가 디코드에 실패한다
  (`RPC: Can't decode result`). 해당 디렉터리 열거가 필요한 모든 작업
  (빌드 stage의 `test/openstep` 열거 등)이 중단된다.
- **범위:** 서버측 단일 함수. 클라이언트·마운트 옵션·네트워크 무관
  (실기에서 `/ndrv`·`/ndrv2` 양쪽 재현). **이전 vnode-wedge 사안과 무관한
  별개 버그다.**

## 2. 근본 원인 (코드·스펙)

`nfs.c nfs_readdir()` 현재 구조:

```c
uint32_t cookie, count;
(void)c; (void)count;                 /* <-- count를 버린다 */
...
xdr_get_u32(in, &cookie); xdr_get_u32(in, &count);
...
need_bytes = 4 + 4 + 4 + strlen(de->d_name) + 4 + 4;
if (xdr_len(out) + need_bytes > 7500)  /* <-- count 대신 상수 7500 */
    break;
```

RFC 1094 READDIR: `readdirargs { fhandle dir; nfscookie cookie; unsigned count; }`
에서 **count = "응답으로 반환될 디렉터리 정보의 최대 바이트 수"**. 서버는
이 상한을 지켜야 하며, 남는 엔트리는 다음 호출(cookie 전진)로 넘긴다.
gnfsd는 이 계약을 어기고 항상 최대 7500바이트를 보낸다.

## 3. 증거 (측정)

### 3.1 서버 동작 — count 무시 (로컬 raw-RPC 프로브)

`test/openstep`(169 엔트리, 총 dir XDR 9208B)에 대해 count를 바꿔가며 READDIR:

| 요청 count | round1 데이터그램 | 판정 |
|-----------|------------------|------|
| 1024 | **7496B** | 응답 > count (위반) |
| 2048 | **7496B** | 응답 > count (위반) |
| 4096 | **7496B** | 응답 > count (위반) |
| 8192 | 7496B | (우연히) count 이내 |

count와 **무관하게 항상 7496B** 반환 → count 무시 확정.

### 3.2 클라이언트 실제 count — NeXTSTEP은 4096

계측 gnfsd(비특권 고포트, READDIR count 로깅)를 띄우고 OPENSTEP에서
`mount -o ...,port=12049`로 NFS만 그쪽에 보내 포착:

```
READDIR count=4096 cookie=0 path=.../scratch/rdtest/d203
```

**NeXTSTEP NFS 클라이언트는 READDIR을 count=4096으로 요청한다.**

### 3.3 클라이언트 디코드 한계 — 데이터그램 ~4960B

8자 고정폭 이름(엔트리당 XDR 24B)으로 크기를 통제한 디렉터리를 실기에서
`ls`(경계 이진탐색):

| 디렉터리 | round1 데이터그램 | 실기 결과 |
|---------|------------------|-----------|
| d200 (202 엔트리) | 4876B | OK |
| d203 (205 엔트리) | **4948B** | **OK** |
| d204 (206 엔트리) | **4972B** | **FAIL (decode)** |
| d350 (>7500 → 캡) | 7496B | FAIL (decode) |

경계는 **4948B(OK) / 4972B(FAIL)**. 요청 count=4096인데 4948B(엔트리
데이터 ~4912B)까지 수용하는 것은, 클라이언트가 count 위에 헤더/여유분을
얹은 버퍼를 잡기 때문. **핵심: count(4096)를 지키면 데이터그램 ≈ 4132B로
한계(4960B) 한참 아래 → 디코드 성공.**

### 3.4 실기 재현 (증상 = 인시던트와 동일)

```
ls /ndrv/openstep-sdl20/test         → rc=0 (엔트리 1개, 작음)          OK
ls /ndrv/openstep-sdl20/test/openstep → "RPC: Can't decode result" ×2   FAIL
ls /ndrv/openstep-nibmaker/.../fontdata(794) → 동일 오류                FAIL
```

### 3.5 왜 8월에 처음

7월 진단 때 walk한 트리(2861파일)에는 단일 READDIR 응답이 클라이언트
count(4096)를 넘길 만큼 큰 디렉터리가 없었다. 8월에 추가된
`openstep-sdl20`(169엔트리 긴 이름의 `test/openstep`, 그 밖에 220·794엔트리
디렉터리)가 잠복 버그를 처음 노출했다. **바이너리 회귀가 아니라 데이터
형태가 바뀌어 드러난 기존 버그.**

## 4. 왜 이 버그인가 (대안 가설 기각)

- **vnode-wedge(FIX_gnfsd.md §3.7)?** 아니다. 새 마운트 `/ndrv2`에서도
  재현되고, umount/재부팅과 무관. 증상도 다르다(busy가 아니라 decode 실패).
- **IP 단편화/rcvbuf(H1/H5)?** 아니다. 작은 디렉터리는 정상, 오직 큰
  디렉터리만 실패. 크기 임계가 정확히 count 경계에 걸린다.
- **핸들/영속화?** 무관. LOOKUP·GETATTR·READ 전부 정상, READDIR만 실패.
- **need_bytes의 XDR 패딩 누락?** `need = 20 + strlen`, 실제 =
  `16 + roundup(strlen,4)`. `need ≥ 실제`(항상 보수적 과대추정)라 7500 캡을
  넘기지 않는다 → 이 자체는 버그 아님. 다만 수정 시 정확히 계산한다(§5).

## 5. 수정 (구체)

### 5.1 핵심 변경 — `count` 존중 + 안전 상한 + 진행 보장

`nfs.c nfs_readdir()`의 캡 로직을 교체한다.

```c
/* nfs.h 또는 nfs.c 상단 */
#define RD_MAXDATA 4096   /* count 존중이 1차 상한. 이 값은 클라가 자기
                             디코드 버퍼보다 큰 count를 보낼 때의 방어 캡:
                             4096이면 어떤 count에도 데이터그램(데이터+~36B)이
                             측정된 NeXTSTEP 한계(~4960B) 아래로 유지된다.
                             (사용자 결정: 8192 대신 4096 채택.) */
```

```c
static int nfs_readdir(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    uint32_t cookie, count;
    DIR *d;
    struct dirent *de = NULL;
    long idx;
    size_t dirbytes;      /* 지금까지 emit한 '디렉터리 데이터' 바이트 */
    size_t cap;
    (void)c;

    if (arg_fh(in, out, &path) < 0)
        return 0;
    if (xdr_get_u32(in, &cookie) < 0 || xdr_get_u32(in, &count) < 0)
        return -1;
    d = opendir(path);
    if (d == NULL) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    xdr_put_u32(out, NFS_OK);

    /* RFC 1094: 응답의 디렉터리 데이터는 count 바이트를 넘지 못한다.
       NeXTSTEP은 count=4096으로 요청하고 그보다 큰 응답을 디코드하지
       못한다("RPC: Can't decode result"). count를 지키되, 비정상적으로
       큰 count가 출력 버퍼를 넘기지 않도록 RD_MAXDATA로 클램프하고,
       count가 한 엔트리보다 작아도 최소 1개는 내보내 진행을 보장한다. */
    cap = count;
    if (cap > RD_MAXDATA)          /* RD_MAXDATA = 4096 */
        cap = RD_MAXDATA;

    idx = 0;
    dirbytes = 0;
    for (;;) {
        struct stat st;
        char child[1024];
        size_t namelen, need_bytes;

        errno = 0;
        de = readdir(d);
        if (de == NULL)
            break;                          /* end-of-dir 또는 에러(errno) */
        idx++;
        if ((uint32_t)idx <= cookie)
            continue;
        namelen = strlen(de->d_name);
        /* 엔트리당 XDR: value-follows(4) + fileid(4)
           + name<>(4 + roundup(namelen,4)) + cookie(4) */
        need_bytes = 4 + 4 + (4 + ((namelen + 3u) & ~(size_t)3u)) + 4;
        if (dirbytes > 0 && dirbytes + need_bytes > cap)
            break;                          /* 더 있음(eof 아님) */
        if (join_child(path, de->d_name, child, sizeof(child)) < 0)
            continue;
        if (lstat(child, &st) < 0)
            continue;
        xdr_put_u32(out, 1);                        /* value follows */
        xdr_put_u32(out, (uint32_t)st.st_ino);      /* fileid */
        xdr_put_bytes(out, de->d_name, (uint32_t)namelen);
        xdr_put_u32(out, (uint32_t)idx);            /* cookie */
        dirbytes += need_bytes;
    }
    xdr_put_u32(out, 0);                    /* no more entries */
    /* eof는 readdir가 진짜 끝(NULL + errno 그대로)일 때만. NULL인데 errno가
       설정됐으면 스캔 도중 read 에러 -> eof 아님으로 보고해 클라이언트가
       마지막 cookie부터 다시 요청하게 한다(꼬리 유실 방지). 크기 캡으로
       break하면 de != NULL이라 자동으로 eof=0. */
    xdr_put_u32(out, (de == NULL && errno == 0) ? 1 : 0);
    closedir(d);
    return 0;
}
```

### 5.2 변경 요점

1. **`count` 존중:** `dirbytes`(엔트리 데이터 누계)가 `cap`을 넘기기 직전에
   멈춘다. count=4096이면 응답 데이터그램 ≈ 4132B로 클라 한계(4960B) 아래.
2. **진행 보장:** `dirbytes > 0` 가드로 **항상 최소 1개 엔트리**를 낸다.
   count가 한 엔트리보다 작아도 무한 루프(0엔트리 eof=0 반복)에 빠지지 않는다.
3. **안전 상한 `RD_MAXDATA`:** count가 거대해도 out 버퍼(65536)를 넘기지
   않게 8192로 클램프. `xdr_put_*`의 반환값을 확인하지 않는 기존 구조에서
   버퍼 오버런→절단→malformed 응답을 원천 차단.
4. **정확한 `need_bytes`:** XDR 이름 패딩(4바이트 정렬)을 반영. `count`
   경계 판정이 실제 와이어 바이트와 일치.
5. **죽은 코드 정리:** `emitted`/`(void)emitted` 제거, `(void)count` 제거.
6. **`de` 초기화:** `de = NULL`로 시작(루프가 한 번도 안 돌 때 eof 판정 안전).
7. **readdir 에러 vs eof 구분(codex Finding 3):** `while ((de=readdir))` 대신
   `for(;;){ errno=0; de=readdir(); ... }`로 바꿔, `readdir`가 NULL을
   반환한 것이 진짜 끝(errno 그대로)인지 read 에러(errno 설정)인지 구분.
   에러면 eof=0으로 보고해 꼬리 유실을 막는다.

### 5.3 count 경계의 의미

`count`는 **디렉터리 데이터(엔트리 목록)** 바이트를 제한한다. RPC/NFS
응답 헤더(24+4B)와 말미(0+eof=8B)는 별도다. 측정상 클라이언트는 count=4096
요청 시 엔트리 데이터 ~4912B까지 수용하므로, 엔트리 데이터를 count(4096)로
묶으면 여유가 충분하다. (헤더까지 포함해 count로 묶는 더 보수적 해석도
가능하나, 측정된 여유를 고려하면 불필요하게 라운드트립만 늘린다.)

## 6. 바꾸지 않는 것 (근거)

- **워커/멀티스레드:** 무관(단일 요청 처리 버그). FIX_gnfsd.md 스트레스에서
  단일스레드가 병목 아님을 이미 확인.
- **rsize/wsize/actimeo/noac 마운트 옵션:** 무관. 이 버그는 옵션과 독립.
- **READDIR O(n²) 재스캔:** 성능 사안이며 이번 정합성 버그와 별개. 측정상
  실사용 문제 없어 유지(FIX_gnfsd.md).
- **`st_ino`의 32비트 절단:** NFSv2 fileid가 u32라 스펙상 불가피. 이번
  버그와 무관하므로 이 변경에 포함하지 않는다.

## 7. 테스트 계획

### 7.0 로컬 PoC 결과 (수정 사본으로 실행 완료)

§5 수정을 scratch 사본에 적용·빌드해 독립 테스트 export로 검증:

| 디렉터리 | 총 엔트리 | 라운드(count=4096) | 최대 데이터그램 | eof |
|---------|----------|--------------------|----------------|-----|
| 169엔트리(긴 이름) | 171(+`.`,`..`) | 3 | 4132B | 도달 |
| 794엔트리 | 796 | 7 | 4132B | 도달 |
| 3엔트리 | 5 | 1 | 136B | 도달 |

- 최대 데이터그램 **4132B** = 엔트리데이터(≤4096) + RPC/NFS 헤더 36B.
  실측 클라 한계 **4948B(OK)** 아래 → NeXTSTEP 안전.
- 794엔트리(실기 FAIL이던 것) 포함 전부 **누락·무한루프 없이 eof 완결**.
- **회귀 통과:** `make test` = ALL PASS(+핸들 영속성), `make stress` =
  STRESS PASS(LINK churn·DRC·32MB 쓰기 무결성).

> 참고: 데이터그램(4132)이 count(4096)보다 36B 큰 것은 RFC의 count가
> **엔트리 데이터**를 제한하기 때문이며(헤더 별도), 측정상 클라가 그만큼
> 여유 버퍼를 잡으므로 안전하다(§9.2에서 codex 검토).

### 7.1 로컬 (OPENSTEP 불필요, 즉시)

- **raw-RPC 프로브**(`readdir_probe.py`)로 수정 후 확인:
  - 모든 요청 count(1024/2048/4096)에 대해 **round1 데이터그램 ≤ count+헤더**.
  - `test/openstep`(169엔트리)가 count=4096에서 **여러 round로 나뉘고
    총 169엔트리·eof=1**로 완결.
  - 794엔트리 디렉터리도 동일하게 완결(무한 루프·누락 없음).
  - count=0/1 같은 극단값에서 **round당 ≥1엔트리**로 진행.
- **회귀:** `make test`(test-nfs.py)·`make stress` 전부 통과.
- **경계 재확인:** 수정 후 로컬에서 d203/d204 상당 디렉터리의 데이터그램이
  count로 묶이는지.

### 7.2 실기 결과 (OPENSTEP, 2026-08-17 — 수정본 gnfsd 기동 후)

수정본을 `serve.sh`로 111/2049에 띄우고 실기에서 인시던트 시나리오 재현:

| 대상 | 수정 전 | 수정 후 |
|------|--------|--------|
| `test`(작음, 대조군) | OK | OK, decode 오류 0 |
| `test/openstep`(169) | **FAIL**(`Can't decode`×2, 0개 반환) | **176 엔트리 나열, decode 오류 0** |
| `fontdata`(794) | **FAIL** | **792 엔트리 나열, decode 오류 0** |
| Mesa src(220) | (미검증) | decode 오류 0 |
| **전체 `/ndrv` 트리 walk** | (대형 디렉터리서 실패) | **13,516 파일 완주, decode 오류 0** |
| 파일 read(READ 회귀) | OK | README 129줄 rc=0, 수정된 디렉터리 내부 파일 1015B read OK |

로컬 프로브로 떠 있는 수정본 확인: `test/openstep`이 count=4096에서
3라운드(84+69+25)로 분할, 최대 데이터그램 4124B(< 클라 한계 4948B), eof 완결.

**결론: 인시던트(`RPC: Can't decode result`) 종단 해소. 회귀 없음.**

> 운영 메모: gnfsd는 export 경로를 **정확 문자열**로 매칭한다(`mnt_mnt`).
> gnfsd를 `/mnt/USERS/.../NeXT_DRIVER`로 띄웠으면 클라이언트도 그 경로로
> 마운트해야 한다(같은 inode라도 `/home/onion/Workspace/NeXT_DRIVER`로
> 요청하면 ACCES). tools의 site.conf와 serve.sh 인자 경로를 일치시킬 것.

## 8. 롤아웃

1. §5 적용 → 로컬 §7.1 통과 확인.
2. codex 재검토 반영.
3. 커밋(GrandCross). `dist/` 재빌드는 배포 절차에 따름.
4. OPENSTEP 가용 시 §7.2 실기 검증 → 인시던트 문서에 결과 반영.

## 9. codex가 집중 검토할 지점 (신뢰하지 말고 교차검증)

1. **진행 보장의 정확성:** `dirbytes > 0` 가드가 모든 경로(스킵/실패 엔트리
   포함)에서 최소 1엔트리를 보장하는가? cookie 전진이 항상 이뤄지는가?
2. **count 경계 해석:** 엔트리 데이터만 count로 묶는 것이 맞는가, 헤더까지
   포함해야 하는가? (측정 여유 vs 스펙 문언)
3. **`RD_MAXDATA` 값:** 8192가 적절한가? out 버퍼·데이터그램·구형 클라
   안전 모두 만족하는가? 더 낮춰야 하나?
4. **`need_bytes` 산식**이 `xdr_put_bytes`의 실제 출력(len4+data+pad)과
   정확히 일치하는가?
5. **스킵 후 eof 판정:** 마지막 엔트리들이 `lstat` 실패로 스킵될 때
   `de == NULL` eof 판정이 잘못 eof=1을 줄 위험은? (실제로는 안전하다고
   보지만 반례를 찾을 것)
6. **cookie 재사용/역호환:** cookie 의미(1-based idx)가 그대로라 기존
   클라이언트와 호환되는가?
7. **회귀:** 이 변경이 DRC·핸들·다른 프로시저에 부작용이 없는가?

## 10. codex 재검토 결과 및 평가 (2026-08-16)

codex(read-only)로 §5 계획을 검토시키고, **신뢰하지 않고** 측정치·RFC
1094·소스와 대조해 판정했다. 5개 finding 중 **1개만 유효**(반영), 최고
심각도 1개는 **오류**(기각), 나머지는 비현실적 지적이었다.

| # | codex 주장 | 심각도(codex) | 내 판정 | 근거 |
|---|-----------|--------------|---------|------|
| 1 | 작은 count일 때 첫 엔트리 강제 emit이 count 위반 → `NFSERR_TOOSMALL` 반환하라 | High | **기각(codex 오류)** | **NFSERR_TOOSMALL은 NFSv2에 없다** — NFS3ERR_TOOSMALL(NFSv3 전용)을 혼동. gnfsd의 NFSv2 에러셋에 부재 확인(grep 0건). 최소 1엔트리 emit은 knfsd·unfsd 등 실서버의 표준 동작이고, 0엔트리+eof=0은 클라 무한루프를 부른다. count=4096인 실클라엔 무해. |
| 2 | `RD_MAXDATA=8192`는 이 클라 한계(~4960)보다 큼 → 향후 큰 count면 재발, 상한 4096 권장 | Medium | **수용(사용자 결정)** | 유효한 핵심: 클라가 자기 버퍼보다 큰 count를 보내면 재발. 처음엔 "count 존중이 계약이니 8192 버퍼안전망"을 권고했으나, 실배포가 NeXTSTEP 단일이고 4096으로 낮춰도 실동작 동일(count=4096이라 cap=min은 어느 쪽이든 4096)이므로 **사용자 결정으로 4096 채택** — 어떤 count에도 데이터그램이 측정 한계 아래로 강제된다. NeXTSTEP엔 실기 검증된 동작과 바이트 동일. |
| 3 | `readdir()` NULL을 무조건 eof로 처리 → 스캔 중 에러 시 꼬리 유실 | Medium | **수용(반영)** | 타당. `readdir` NULL은 끝과 에러를 겸한다. `errno`로 구분하도록 §5.2-7 반영. 저비용·정합성 개선. |
| 4 | `idx`(long) vs cookie(uint32) — 초대형 디렉터리서 UB/wrap | Low | **기각(비현실)** | 단일 디렉터리 2^31개 엔트리는 실파일시스템서 불가능. 64bit host라 long 오버플로 도달 불가. 기존 코드 특성이며 무해. |
| 5 | `namelen+3` wrap 가능 → 255 상한 명시 | Low | **기각(비현실)** | `d_name`은 NAME_MAX(255) 이내. `strlen`이 SIZE_MAX 근처일 수 없어 wrap 불가. |

**종합:** codex는 유효 지적 1건(Finding 3, 반영)과 **명백한 오류 1건**
(Finding 1 — NFSv2에 없는 에러 코드 제안)을 냈다. 핵심 수정(count 존중)은
codex 검토 후에도 유효하다. Finding 3 반영본으로 `make test`·`make stress`
재통과, count 존중·대형 디렉터리 완결 재확인.

**결정 완료:** Finding 2의 `RD_MAXDATA`는 **4096 채택**(사용자 결정).

**회귀 테스트 추가:** `test-nfs.py`에 대형 디렉터리(200엔트리) READDIR을
count=1024로 돌려 (a) 각 라운드 데이터 ≤ count, (b) 빈 non-eof 라운드 없음,
(c) 여러 라운드 분할, (d) 전 엔트리 반환 + eof를 검증하는 5개 케이스 추가.
`make test` 31/31 통과. (테스트 작성 중 발견: 서브디렉터리를 mode 0644로
MKDIR하면 실행비트가 없어 그 안 파일 생성이 EACCES가 된다 — gnfsd의
정상 POSIX 동작. 테스트는 0755로 생성하도록 수정.)
