# 어차피 메모리에서 읽는다면?

MySQL에서 커버링 인덱스를 잘 잡으면 조회가 꽤 빨라집니다.

그래서 이런 의문이 생긴다.

> 인덱스도 결국 버퍼 풀에 올라와서 메모리에서 읽는 상황이라면, Redis 캐시와 뭐가 그렇게 다를까?

이번 글은 그 질문에 답하기 위해 적은 글입니다.

~~다만 실험을 하다보니 위 질문보다는 다른 인사이트를 더 많이 얻게 된거 같습니다..~~

목차

1. 사전 지식
2. 커버링 인덱스 설명
3. Redis 구조 설명
4. 실험 결과
5. 배운 점 / 실무에서 느껴야 할 감각

## 1. 사전 지식

### MySQL도 충분히 "메모리에서 읽는 상태"가 될 수 있다

MySQL InnoDB는 버퍼 풀(buffer pool)에 테이블/인덱스 페이지를 캐시합니다.
즉 자주 읽는 인덱스는 이미 메모리에 올라와 있을 수 있습니다.

Redis가 빠른 이유를 단순히 "Redis는 메모리, MySQL은 디스크"라고 하기엔 좀 아쉬운 답변이죠.

### 하지만 버퍼 풀은 결과 캐시가 아니다

버퍼 풀은 조회 결과를 저장하는 건 아닙니다.
저장하는 것은 페이지(page)죠.

- 테이블 데이터 페이지
- 인덱스 B+Tree의 루트/중간/리프 페이지

즉 버퍼 풀에 올라와 있다는 말은,
"재료는 메모리에 있다"는 뜻이지 "요리가 완성돼 있다"는 뜻은 아닙니다.

### "인덱스 파일 전체가 메모리에 있다"보다 중요한 건 working set?

메모리에서 읽는 상황을 만드려면 다음을 알아야 합니다.
"이 쿼리가 실제로 쓰는 relevant page working set이 버퍼 풀에 올라와 있는가?"

> relevant page working set: 이 쿼리를 처리할 때 실제로 필요한 InnoDB 페이지들의 집합

relevant page는 보통 이런 것들입니다.

- 해당 인덱스의 root page
- 조건 범위로 내려가기 위한 internal page들
- 조건 범위의 leaf page들
- PK lookup에 필요한 clustered index page
- covering이면 보통 clustered lookup page는 거의 필요 없음

즉 relevant page working set이 버퍼 풀에 올라와 있다는 말은,
**이 쿼리가 실제로 건드리는 페이지들이 이미 메모리에 있어서 디스크를 다시 안 읽어도 된다**는 뜻입니다.

InnoDB는 파일 전체를 통째로 올리는 게 아니라 페이지 단위로 캐시합니다.
따라서 이번 비교에서 중요한 것은 "인덱스 파일 전체가 메모리에 있느냐"보다, 이 쿼리 수행에 필요한 페이지들이 버퍼 풀 hit만으로 처리되느냐입니다.

### 이번 실험 대상 조회

실험에서는 아래 조회 하나만 고정해서 비교했습니다.

```sql
SELECT user_id, match_score
FROM user_match
WHERE university_id = 1
  AND gender = 'M'
ORDER BY match_score DESC
LIMIT 20;
```

이 쿼리를 고른 이유는...

- 조건이 단순
- 정렬이 있음
- 반환 컬럼이 적음
- 상위 N개 조회라 캐시 대상이 되기 쉬움

비교 대상은 세 가지다.

- `plain`: 비커버링 인덱스
- `covering`: 커버링 인덱스
- `redis`: 결과 JSON 캐시

## 2. 커버링 인덱스 설명

먼저 비커버링 인덱스를 봐봅시다.

```sql
(university_id, gender, match_score DESC)
```

이 인덱스는 `WHERE university_id = 1 AND gender = 'M'` 조건과  
`ORDER BY match_score DESC`를 꽤 잘 처리할 수 있습니다.

문제는 `SELECT user_id, match_score`에서 `user_id`가 인덱스에 없다는 점이죠.

InnoDB에서 보조 인덱스는 leaf node에 보조 키와 PK 값을 저장합니다.
그래서 비커버링 인덱스는 보조 인덱스로 후보를 찾은 뒤, 다시 PK를 이용해 클러스터드 인덱스를 따라가 실제 행을 확인해야 ~~합니다.~~ 할 수도 있습니다.

이 추가 단계가 흔히 말하는 table lookup 비용입니다.

반면 커버링 인덱스는

```sql
(university_id, gender, match_score DESC, user_id)
```

여기서는 조회에 필요한 컬럼이 인덱스 안에 모두 들어 있습니다.
즉 MySQL이 인덱스 leaf만 보고 결과를 만들 수 있죠.

커버링 인덱스가 줄여주는 비용은 명확합니다.

- 클러스터드 인덱스 재조회 감소
- 추가 페이지 접근 감소
- 랜덤 lookup 감소
- `Using index` 가능

즉 커버링 인덱스는 DB가 덜 일하게 만듭니다.

하지만 여기서 멈출수 없죠.
커버링 인덱스는 table lookup을 줄일 뿐, **쿼리 처리 자체**를 없애지는 않으니까요

MySQL은 여전히 다음 일을 해야 합니다.

- SQL 실행
- 인덱스 트리 탐색
- 조건 구간 찾기
- 리프 스캔
- 결과 조립

즉 커버링 인덱스는 "조회 비용 절감"이고, "결과 재사용"은 아니다.

## 3. Redis 구조 설명

이제 Redis를 보자.

이번 실험에서 Redis는 아래처럼 썼습니다.

- 키: `user_match:u=1:g=M:limit=20`
- 값: 조회 결과 20건 전체를 담은 JSON 문자열

즉 Redis는 이번 요청에서 SQL을 실행하지 않는다.  
이미 계산해 둔 결과를 키 하나로 바로 반환합니다.

Redis의 흐름은 훨씬 단순하다.

1. 키를 해시 테이블에서 찾는다
2. 값 객체를 꺼낸다
3. 그 값을 그대로 반환한다

이 구조가 중요한 이유는, Redis가 들고 있는 것이 **인덱스 페이지**가 아니라 **완성된 결과**라는 점입니다.

같은 "메모리"라고 해도 성격은 완전히 다르죠

- InnoDB 버퍼 풀: 페이지 캐시
- 커버링 인덱스: 결과를 만들기 위한 접근 경로
- Redis 캐시: 최종 결과 자체

즉 Redis가 빠른 이유는 단순히 DRAM에 있어서가 아니라,
결과를 다시 계산하지 않고 바로 재사용하기 때문입니다.

## 4. 실험 결과

명확한 실험 해석을 위해 버퍼 풀 조건을 명시적으로 통제했습니다.

### 실험 환경

- MySQL 8.0.36
- Redis 7.2
- Docker Compose
- Python 적재/벤치 스크립트
- 데이터 `2,000,000`건
- 조회 조건 매칭 rows: `405,919`
- 매칭 비율: `20.30%`
- `user_match` 테이블 크기: 약 `182.31MB`
  - data: `112.66MB`
  - index: `69.66MB`
- MySQL `innodb_buffer_pool_size`: `256MB`
- Redis `maxmemory`: `256MB`

이 설정은 "둘 다 메모리에서 읽는다면" 이라는 조건에 집중해서 설정했습니다.

메모리 예산 자체를 맞춰 놓고,
그 위에서 페이지 캐시 기반 조회와 결과 캐시 기반 조회가 어떻게 달라지는지를 보겠습니다..

다만 같게 맞춘 것은 메모리 상한선이지, 실제 점유 메모리 양 자체는 아니다.

- MySQL은 `user_match`와 인덱스 working set을 버퍼 풀에 유지한다
- Redis는 이번 실험에서 고정 조회 결과 1개만 캐싱했다

실제로 측정 직후 Redis 사용 메모리는 약 `1MB` 수준이었습니다.

즉 이번 비교는
고정된 hot query 하나를 결과 캐시로 분리했을 때 어떤 구조적 차이가 생기는가를 보기위한 실험입니다.

실행 계획 확인

- `plain`: `Using index condition`
- `covering`: `Using where; Using index`

즉 `plain`은 인덱스를 타지만 결과를 인덱스만으로 끝내지 못했고, `covering`은 인덱스만으로 결과를 만들 수 있었습니다.

### cold / warm 분리

- `innodb_buffer_pool_dump_at_shutdown=OFF`
- `innodb_buffer_pool_load_at_startup=OFF`
- `cold`: 인덱스를 준비한 뒤 MySQL을 재시작하고, warmup 없이 측정
- `warm`: 인덱스를 준비한 뒤 MySQL을 재시작하고, warmup 후 측정
- 최종 warm 수치는 `benchmark_buffer_pool_reads_delta=0`인 run만 채택

즉 warm 비교는 적어도 측정 구간 동안 InnoDB buffer pool miss 없이 수행된 run만 사용했습니다.

### 시나리오

| scenario | description | requests | warmup | concurrency |
|---|---|---:|---:|---:|
| interactive_single | 단건 응답 시간 위주. 사용자 한 명이 반복 조회 | 3000 | 300 | 1 |
| steady_api | 일반적인 API 핫패스 | 8000 | 500 | 8 |
| burst_hot_key | 같은 조회가 짧은 시간에 몰리는 버스트 | 16000 | 1000 | 32 |

### warm verified 결과

| scenario | case | avg_ms | p95_ms | max_ms | tps |
|---|---|---:|---:|---:|---:|
| interactive_single | plain_warm | 0.713 | 1.261 | 16.076 | 1399.53 |
| interactive_single | covering_warm | 0.685 | 1.191 | 8.661 | 1455.71 |
| interactive_single | redis | 0.395 | 0.741 | 13.700 | 2530.21 |
| steady_api | plain_warm | 3.093 | 6.302 | 19.282 | 2566.21 |
| steady_api | covering_warm | 5.423 | 11.958 | 103.447 | 1468.71 |
| steady_api | redis | 2.409 | 5.638 | 261.028 | 3306.56 |
| burst_hot_key | plain_warm | 12.483 | 21.519 | 59.326 | 2518.03 |
| burst_hot_key | covering_warm | 11.325 | 22.966 | 75.720 | 2775.43 |
| burst_hot_key | redis | 3.992 | 8.768 | 144.396 | 7902.94 |

그리고 warm MySQL 최종 비교는 모두 `benchmark_buffer_pool_reads_delta=0`을 만족했습니다.

즉, 버퍼 풀에 relevant page가 올라온 MySQL"과 Redis를 놓고 본 비교입니다.

- `interactive_single`: plain 0, covering 0
- `steady_api`: plain 0, covering 0
- `burst_hot_key`: plain 0, covering 0

이 수치를 볼 때 한 가지를 더 봐봅시다.

- `user_match` 전체는 약 `182MB`
- MySQL 버퍼 풀은 `256MB`

즉 이번 MySQL 수치는 거의 "메모리 위에서 낼 수 있는 최선에 가까운 상태"라고 봐도 됩니다.
이번 실험에서 Redis가 이긴다면, 그건 디스크 I/O 차이라기보다 처리 경로 차이 때문이라고 해석하는 편이 맞을거 같네요

### 결과 해석

#### 1. 단건 저부하에서는 MySQL 자체가 이미 충분히 빠르다

`interactive_single`에서는 warm plain과 warm covering 차이가 거의 없다.

- plain_warm avg: `0.713ms`
- covering_warm avg: `0.685ms`
- redis avg: `0.395ms`

Redis가 제일 빠르긴 해요
하지만 차이는 `0.29ms` 수준이다.

즉 이 구간에서 중요한 질문은

> "더 빠르냐?"가 아니라 "이 차이가 캐시 복잡도를 감당할 만큼 큰가?"다.

이 결과는 단순한 조회, 작은 결과 집합, 낮은 동시성에서는 MySQL만으로도 충분히 실용적일 수 있다는 가설과 잘 맞습니다.

왜 이렇게 나오는지 보기위해 쿼리를 까보면..

`ORDER BY match_score DESC LIMIT 20`
MySQL은 조건에 맞는 구간에 진입한 뒤 상위 `20`건만 찾으면 빠르게 찾아줍니다.

그래서 `plain`이 추가로 치르는 비용은 사실상 상위 20건 정도를 PK로 다시 확인하는 비용에 가까운거죠.

그 lookup이 디스크가 아니라 버퍼 풀 hit라면,
절대 시간은 생각보다 크지 않네요.

그래서 이 구간에서는 커버링 인덱스가 있든 없든 MySQL 자체가 이미 충분히 빠릅니다.

#### 2. 커버링 인덱스가 항상 plain보다 빠른 건 아니네요..

이번 실험에서 제일 흥미로운 구간은 `steady_api`였습니다.

- plain_warm avg: `3.093ms`
- covering_warm avg: `5.423ms`
- plain_warm p95: `6.302ms`
- covering_warm p95: `11.958ms`

즉 이번에는 오히려 `plain`이 더 빨랐다.

이건 B+Tree 관점으로 봐야 이해가 쉽습니다.

비커버링 보조 인덱스의 leaf에는 대략 이런 정보가 들어 있습니다.

- `university_id`
- `gender`
- `match_score`
- PK

반면 커버링 인덱스 leaf에는 여기에 `user_id`가 하나 더 들어가죠.

즉 커버링 인덱스는 lookup을 줄여주는 대신, leaf entry를 더 넓게 만듭니다.

그리니까..

- 같은 `16KB` 페이지에 들어가는 entry 수가 줄고
- leaf page 밀도가 낮아지고
- 같은 범위를 읽을 때 더 많은 페이지를 건드릴 수 있고
- CPU cache locality와 메모리 밀도가 나빠질 수 있다

원래 커버링 인덱스의 장점은 분명하다.

- PK 기반 row lookup 제거
- clustered index 재조회 제거
- 랜덤 접근 감소

그런데 이번 쿼리는 `LIMIT 20`이죠?
즉 `plain`이 추가로 부담하는 row lookup 수 자체가 매우 작은거죠

그리고 그 lookup이 버퍼 풀 hit라면 penalty가 더 줄어듭니다.
그러면 이 구간에서는

- 커버링이 줄여주는 lookup 비용

보다

- 커버링이 늘려버린 인덱스 폭 비용

이 더 크게 보일 수 있다.

심지어 Redis는 여기서 한 단계 더 효율적이죠.

- redis avg: `2.409ms`
- redis p95: `5.638ms`

결론적으로,
`커버링 인덱스 = 항상 제일 빠름`이 아니라,
**메모리 위에서의 비용 구조가 달라지면 좁은 인덱스가 더 유리할 수도 있다**는 걸 알고 있어야 하는거죠.

즉 커버링 인덱스는 "무조건 fastest"가 아니라, row lookup이 비싼 상황에서 강한 전략이라고 보는 편이 맞는거죠.

### 버스트에서는 Redis가 구조적인 차이를 만들어

`burst_hot_key`에서는 차이가 더 분명합니다.

- plain_warm avg: `12.483ms`
- covering_warm avg: `11.325ms`
- redis avg: `3.992ms`

- plain_warm p95: `21.519ms`
- covering_warm p95: `22.966ms`
- redis p95: `8.768ms`

여기서는 MySQL 둘 다 메모리에서 읽고 있음에도,
Redis가 tail latency에서 확실히 앞섭니다.

특히 같은 hot key가 반복될 때는 MySQL은 매번 질의를 다시 처리하고, Redis는 이미 만들어 둔 결과를 그대로 줍니다.

MySQL은 버퍼 풀 hit 상태에서도 여전히

- SQL 실행
- 인덱스 탐색
- 결과 조립
- 세션/엔진 처리

를 계속 해야 합니다.

반면 Redis는 결과를 키 하나로 바로 꺼내는데, 결과를 재사용 하니 뭐 당연한거죠.

이걸 요청 처리 경로로 더 단순화하면 이렇다.

MySQL:

1. SQL 요청 수신
2. 실행기 진입
3. 인덱스 range 탐색
4. leaf scan
5. 결과 20건 조립
6. protocol row 직렬화
7. 클라이언트 전송

Redis:

1. key lookup
2. value fetch
3. 이미 만들어진 JSON 반환

둘 다 메모리에서 읽더라도, MySQL은 요청마다 질의를 다시 수행하고, Redis는 완성된 결과를 재사용합니다.

그래서 burst에서는 단순히 평균 처리 시간만 차이 나는 것이 아니라, 대기열이 쌓이는 방식 자체가 달라지네요.

동시성 `32`에서는 tail latency가 보통

- 실제 처리 시간
- 앞선 요청을 기다린 시간

의 합으로 커지죠.

여기서 Redis는 요청당 서비스 시간이 더 짧아서 뒤 요청들이 덜 밀리고, 그 결과 p95에서 차이를 크게 벌립니다.

버스트 상황에서 `avg` 차이보다 `p95` 차이가 더 중요한 이유는, 큐잉이 붙으면서 지연이 비선형적으로 커지기 때문입니다.

이번 결과에서 주목할 점은
`plain`과 `covering`의 우열이 아니라, 둘 다 MySQL은 `10ms`대 avg, `20ms`대 p95이고 Redis는 `3ms`대 avg, `8ms`대 p95라는 거...

## 배운 점

### 커버링 인덱스는 좋은 최적화지만, 무조건 정답은 아니다

다음 조건이면 우선 MySQL에서 끝내는 편이 맞을 거 같아요..

- 조회 패턴이 단순하다
- 반환 행 수가 작다
- warm 기준 avg/p95가 이미 충분히 낮다
- 최신성이 중요하다
- 캐시 무효화 복잡도를 늘리고 싶지 않다

그리고 이번 실험은 한 가지를 더 보여줍니다.

> 커버링 인덱스는 row lookup을 없애지만, 인덱스를 더 넓게 만든다.

그래서 메모리 hit 위주, 작은 `LIMIT`, 작은 결과 집합이라면
좁은 비커버링 인덱스가 오히려 더 나을 수도 있습니다.

### Redis는 "메모리라서"가 아니라 "질의 처리를 우회해서" 빠르다

Redis를 붙이는 순간 얻는 이점은 미세 최적화가 아니다.

- SQL 실행 우회
- 인덱스 탐색 우회
- 결과 재조립 우회
- hot key 반복 요청 흡수

그래서 같은 조회가 반복되는 구간에서는 Redis가 훨씬 큰 차이를 만든다.

### 퍼센트보다 절대 시간이 중요하다

- `0.685ms -> 0.395ms`
- `11.325ms -> 3.992ms`

둘 다 개선이지만 분명 의미는 다르죠?

전자는 "좋긴 한데 굳이?"일 수 있고,  
후자는 "구조적으로 바꿀 이유가 있다"에 가깝겠죠.

### p95가 무너지면 Redis 도입 명분이 커진다

p95, p99 같은 tail latency에서 redis의 필요가 생깁니다.

이번 실험에서도 버스트 구간에서 Redis는 p95를 크게 낮춥니다.

- covering_warm p95: `22.966ms`
- redis p95: `8.768ms`

## 이번 실험은 왜 Redis가 더 빠른지를 분석하려고 시작했지만..

오히려 제가 배운건

- 커버링 인덱스는 행 재조회 비용을 줄이지만, 인덱스를 넓힌다
- Redis 캐시는 질의 처리 자체를 우회한다
- 그리고 메모리 예산을 맞춘 뒤에도 그 차이는 남는다

즉 셋은 모두 "메모리 활용"처럼 보이지만, 실제로 줄이는 비용의 종류가 다릅니다.
