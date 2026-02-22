# 배치 작업과 MySQL 격리수준

MySQL 격리수준별로 배치 작업이 읽고 -> 계산하고 -> 쓰는 동안, 동시에 OLTP 트래픽이 들어오면 어떤 일이 생길까?
_(OLTP: Online Transaction Processing, 온라인 트랜잭션 처리)_

흔히들..
- 중복 처리
- 누락 처리
- 합산/집계가 틀어짐
- 락 대기 / 데드락 / 타임아웃
  
이상현상(사실 사용자 입장에서의 이상현상인거지 DB입장에선 정상임,,)이라고 하는데

그래서 MySQL 격리수준(Read Committed / Repeatable Read / Serializable) 별로, 이런 현상이 어떻게 관측되는지 실험하고 숫자로 기록 해봤습니다.

> 실험 설계, 해석에 GPT가 정말 많이 도움 됐습니다 ㅎㅎ

---

## 출발점(궁금증)

> 격리수준을 올리면 정합성은 좋아지겠지… 근데 운영은 더 빡세지지 않을까?

즉,정합성(중복/누락/오차) vs 운영비용(대기/데드락/타임아웃/재시도)의 트레이드오프 속에서 

**격리수준이 답인지, 아니면 설계 패턴이 답인지**
확인해보고 싶었습니다.

---

## 격리수준을 아주 짧게(진짜로 짧게)

> 아래 요약은 MySQL InnoDB를 기준으로 잡았습니다. (세부는 케이스/쿼리 형태에 따라 달라집니다)


- **READ UNCOMMITTED**  
  커밋하지 않은 데이터 조차도 접근할 수 있는 격리 수준 -> 다른 트랜잭션의 작업이 커밋 또는 롤백되지 않아도 즉시 보이게 된다.  

- **READ COMMITTED (RC)**  
  각 statement가 “그 시점에 커밋된 최신값”을 보기 쉬움 -> 같은 트랜잭션 안에서 2번 읽으면 값이 달라질 수 있음(Non-repeatable read).


- **REPEATABLE READ (RR, MySQL 기본값)**  
  트랜잭션 시작 시점(또는 최초 consistent read 시점) 스냅샷을 유지 -> 같은 트랜잭션 안의 재조회는 보통 안정적.  
  대신 범위/락 패턴에 따라 **gap/next-key**로 경합이 커질 수 있음.


- **SERIALIZABLE**  
  정합성은 강하지만, 대가로 동시성이 크게 줄 수 있음.  
  실제로는 “읽는 쪽이 쓰는 쪽을 막거나”, 그 반대가 되어 **락 대기/타임아웃/데드락**이 늘어나는 모습으로 자주 보임.
---

## 가설

- **가설 A**: RC에서 집계 배치는 동시 업데이트/삽입 때문에 합계가 틀어지고, 배치 내 스냅샷이 없으면 diff가 커진다.
- **가설 B**: RR은 읽기 일관성은 좋아지지만, 장시간 트랜잭션이면(특히 MySQL) gap/next-key로 범위 경합이 커져 지연이 증가한다.
- **가설 C**: Serializable은 정합성은 강하지만, 충돌/대기로 인해 실패(타임아웃)나 재시도가 늘어 wall time이 늘거나 실패율이 오른다.
- **가설 D**: 격리수준이 낮아도 패턴(SKIP LOCKED, dedupe 유니크, upsert/멱등)으로 정합성을 맞출 수 있다.

---
## 실험 환경

- MySQL 8.0.36 (docker)
- `orders` 약 2.1억건(최대 id 기준 `209,663,440`)
- `autocommit=1` (하지만 실험은 `START TRANSACTION`으로 명시적 트랜잭션)
- 기본 격리수준: `REPEATABLE-READ`
- `innodb_lock_wait_timeout=5`

---

## 가설 A) RC 집계 틀어짐
**:“같은 배치인데 합이 달라요”**
~~READ COMMITED를 이해하고 있다면 당연한 이야기ㅋㅋ~~
### 실험 A-1) 같은 트랜잭션에서 `SUM(amount)`를 두 번 읽으면(그 사이 OLTP 커밋) 뭐가 보일까?

### 궁금증
집계 배치가 한 트랜잭션 안에서 `SUM(amount)`를 두 번 읽을 때, 그 사이에 OLTP가 업데이트를 커밋하면 합계가 진짜로 틀어질까?

### 결과(요약)

`range_size=1,000,000` 기준으로 딱 예쁘게 이렇게 나옴:

RC:

```
batch_sum_diff = 800000  (RC)
```

RR:

```
batch_sum_diff = 0  (RR)
```

### 왜 그럴까?

- RC는 “그 시점에 커밋된 최신값”을 다시 볼 수 있어서, 같은 트랜잭션이어도 2번째 SELECT가 달라질 수 있습니다.
- RR은 스냅샷을 유지해서, 같은 트랜잭션 안에서는 재조회가 안정적입니다.

포인트:

- RR이 정답을 보장한다기보단, 배치 1회 안에서의 일관성을 보장해주는 것입니다.
- 대신 배치가 길어지면 운영비용(경합/지연)이 커질 수 있으니(가설 B), 범위/시간/패턴 설계가 필요합니다.

---

### 실험 A-2) 범위 크기에 따른 트레이드오프: 정합성(배치) vs 운영비용(OLTP)
> _사실 이게 진짜_

수행

```bash
bash tools/run_orders_range_sweep.sh "100000,1000000" 1 "READ COMMITTED,REPEATABLE READ"
bash tools/run_orders_range_sweep.sh "10000000,50000000" 1 "READ COMMITTED,REPEATABLE READ,SERIALIZABLE"
```
<details><summary>sh 스크립트 속 sql 스크립트</summary>

`agg_range_demo_batch.sql`

```sql
SET @sql := CONCAT('SET SESSION TRANSACTION ISOLATION LEVEL ', @isolation);
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

START TRANSACTION;
DO GET_LOCK('agg_range_start', 30);
DO GET_LOCK('agg_range_gate', 30);
DO RELEASE_LOCK('agg_range_start');

SELECT SUM(amount), COUNT(*) INTO @sum1, @cnt1
FROM orders
WHERE id BETWEEN @lo_id AND @hi_id AND status='PAID';

DO RELEASE_LOCK('agg_range_gate'); -- OLTP가 이제 UPDATE 하러 들어옴
DO GET_LOCK('agg_range_done', @done_timeout_seconds); -- OLTP 커밋까지 대기(선택)
DO RELEASE_LOCK('agg_range_done');

SELECT SUM(amount), COUNT(*) INTO @sum2, @cnt2
FROM orders
WHERE id BETWEEN @lo_id AND @hi_id AND status='PAID';
COMMIT;

SELECT (@sum2-@sum1) AS sum_diff;
```

OLTP 쪽(발췌: `experiments/20_agg_snapshot/agg_range_demo_oltp.sql`):

```sql
DO GET_LOCK('agg_range_done', 30);  -- 커밋까지 잡고 있어서 batch가 기다릴 수 있음
DO GET_LOCK('agg_range_start', 30); DO RELEASE_LOCK('agg_range_start');
DO GET_LOCK('agg_range_gate', 30);  DO RELEASE_LOCK('agg_range_gate');

START TRANSACTION;
UPDATE orders
SET amount = amount + 1, updated_at = NOW()
WHERE id BETWEEN @lo_id AND @hi_id AND status='PAID';
COMMIT;

DO RELEASE_LOCK('agg_range_done');
```

포인트:

- RC면 두 번째 SELECT가 OLTP 커밋을 볼 수 있어서 `sum_diff > 0`이 나오기 쉬움
- RR이면 같은 tx 안 consistent read 스냅샷이라 `sum_diff = 0`이 나오기 쉬움
</details>


**결과**

| range_size | isolation | lo_id | hi_id | wall_s | batch_sum_diff | oltp_rows_updated | oltp_waited_ms | oltp_status | deadlocks_delta | lock_wait_timeouts_delta |
|---:|---|---:|---:|---:|---:|---:|---:|---|---:|---:|
| 100000 | READ COMMITTED | 1 | 100000 | 3 | 80000.000000000000000000000000000000 | 80000 | 398.5330 | OK | 0 | 0 |
| 100000 | REPEATABLE READ | 1 | 100000 | 2 | 0.000000000000000000000000000000 | 80000 | 364.4230 | OK | 0 | 0 |
| 1000000 | READ COMMITTED | 1 | 1000000 | 6 | 800000.000000000000000000000000000000 | 800000 | 3314.6630 | OK | 0 | 0 |
| 1000000 | REPEATABLE READ | 1 | 1000000 | 6 | 0.000000000000000000000000000000 | 800000 | 3383.5090 | OK | 0 | 0 |
| 10000000 | READ COMMITTED | 1 | 10000000 | 39 | 7650368.000000000000000000000000000000 | 7650368 | 32659.9040 | OK | 0 | 0 |
| 10000000 | REPEATABLE READ | 1 | 10000000 | 43 | 0.000000000000000000000000000000 | 7650368 | 32013.3720 | OK | 0 | 0 |
| 10000000 | SERIALIZABLE | 1 | 10000000 | 46 | 0.000000000000000000000000000000 | 7650368 | 39120.5250 | OK | 0 | 0 |
| 50000000 | READ COMMITTED | 1 | 50000000 | 197 | 38174144.000000000000000000000000000000 | 38174144 | 167867.2940 | OK | 0 | 0 |
| 50000000 | REPEATABLE READ | 1 | 50000000 | 215 | 0.000000000000000000000000000000 | 38174144 | 165694.0060 | OK | 0 | 0 |
| 50000000 | SERIALIZABLE | 1 | 50000000 | 48 | 0.000000000000000000000000000000 | NA | NA | 1205 | 0 | 1 |

> _range_size: 이번 실험에서 배치/OLTP가 부딪히는 orders.id 범위 크기(행 수)._
> _isolation: 세션 격리수준(READ COMMITTED / REPEATABLE READ / SERIALIZABLE)._
> _lo_id, hi_id: 테스트 대상 orders.id 범위._
> _wall_s: 배치+OLTP를 한 “세트”로 돌린 총 wall time(초)._
> _batch_sum_diff: 같은 배치가 범위를 두 번 읽었을 때 SUM 차이(두 번째 SUM - 첫 번째 SUM)._
> _oltp_rows_updated: OLTP UPDATE가 실제로 건드린 행 수(= status='PAID' 매칭 수)._
> _oltp_waited_ms: OLTP UPDATE 트랜잭션이 시작~종료(커밋 또는 실패)까지 걸린 시간(ms, 실행+대기 포함)._
> _oltp_status: OLTP 결과 상태(OK=커밋 성공 / 1205=lock wait timeout 등 에러코드)._
> _deadlocks_delta: 실험 전/후로 관측된 데드락 카운트 증분(벤치 러너 기준)._
> _lock_wait_timeouts_delta: 실험 전/후로 관측된 lock wait timeout 카운트 증분(벤치 러너 기준)._

해석:

- RC는 범위가 커질수록 `batch_sum_diff`가 커집니다. (배치 1회 안에서 SUM이 “흔들림”)
  - 특히 이 실험은 OLTP가 `amount=amount+1`을 해서, **RC의 sum_diff가 “업데이트된 row 수”만큼** 커지는 게 아주 잘 보였습니다.
- RR은 범위가 커져도 `batch_sum_diff=0`으로 “배치 내부 일관성”은 잘 지켜집니다.
- (스포) SERIALIZABLE은 큰 범위(50M)에선 OLTP가 `1205(lock wait timeout)`로 실패했습니다.

---

### 실험 A-3) 집계 배치를 chunk commit으로 하면 “한 배치 결과가 mixed snapshot”이 될 수 있다

이건 “격리수준” 보다 “배치 설계(커밋 단위)” 쪽 실험입니다

실행(안전하게 `bench_orders`만 씁니다):

```bash
bash tools/run_chunk_demo.sh 10000
```

결과(캡처):

```text
chunk1_sum=500000
chunk2_sum=505000
mixed_total=1005000

truth_sum=1010000
diff(mixed-truth)=-5000
```


> chunk1은 업데이트 “전”, chunk2는 업데이트 “후”를 읽어서 한 번의 배치 결과가 업데이트 전/후가 섞인(mixed snapshot) 값이 될 수 있음


---

## 가설 B) RR 장시간 트랜잭션 비용(MySQL스럽게): gap/next-key + undo/purge

### 실험 B-1) RR/SERIALIZABLE에서 gap/next-key로 범위 경합이 진짜 커지나?

가설 B를 숫자로 확인해보고 싶어서, 작은 벤치 테이블을 만들었습니다.

- 홀더(Session 1): 인덱스 범위를 `FOR UPDATE`로 잡고 5초 동안 버티기
- 인서터(Session 2): 그 범위에 INSERT 시도하기

실행:

```bash
bash tools/run_gaplock_bench_nodrop.sh 100 200 101 5
```

결과:

| isolation | inserter_waited_ms | ps_row_lock_waits_during | ps_max_wait_age_secs_during | deadlocks_delta | lock_wait_timeouts_delta |
|---|---:|---:|---:|---:|---:|
| READ COMMITTED | 11.1370 | 0 | 0 | 0 | 0 |
| REPEATABLE READ | 4659.6510 | 1 | 1 | 0 | 0 |
| SERIALIZABLE | 4668.1340 | 1 | 1 | 0 | 0 |

> _isolation: 세션 격리수준._
> _inserter_waited_ms: INSERT 시도 트랜잭션이 끝날 때까지 걸린 시간(ms, 실행+대기 포함)._
> _ps_row_lock_waits_during: 실행 구간 동안 performance_schema에서 관측된 row lock wait 이벤트 수(요약)._
> _ps_max_wait_age_secs_during: 실행 구간 동안 “가장 오래 대기 중”이었던 lock wait의 age 최대값(초)._
> _deadlocks_delta: 실험 전/후 데드락 카운트 증분._
> _lock_wait_timeouts_delta: 실험 전/후 lock wait timeout 카운트 증분._

RC: 바로 들어가세요~(11ms)  
RR/SERIALIZABLE: 잠깐만요… 줄 서세요(~4.6s)

→ 범위 락(gap/next-key)이 실제로 INSERT를 막는 걸 확인했습니다.

---

### 실험 B-2) RR “장시간 스냅샷”이 undo/purge(History list length)를 악화시키는지 보기

가설 B의 포인트(in MySQL):

> 오래 열린 RR 트랜잭션(스냅샷)이 있으면, purge가 옛 버전을 못 지워서 undo backlog(History list length)가 쌓일 수 있다.

그래서 같은 업데이트 부하를 두 번 돌렸습니다.

- `baseline`: long snapshot 없이 업데이트 부하
- `rr_snapshot`: RR 트랜잭션을 60초 동안 “열어둔 채” 업데이트 부하
- `cooldown`: snapshot 커밋 후 purge가 따라잡는 구간(30초 관찰)

실행:

```bash
bash tools/run_undo_history_len_bench.sh 60 60 4 20000 1 1000000 2 30
```
<details><summary>sql 스크립트</summary>
Lock 점유

```sql
SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;
START TRANSACTION;
SELECT COUNT(*) FROM orders WHERE id BETWEEN @lo_id AND @hi_id;
DO SLEEP(@snapshot_hold);
COMMIT;
```

업데이트 부하
```sql
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
START TRANSACTION;
UPDATE orders o
JOIN (
  SELECT (@lo_id + ((n + @wid * 97) % (@hi_id - @lo_id + 1))) AS id
  FROM nums_1m
  WHERE n <= @chunk
) t USING (id)
SET o.amount = o.amount + 1,
    o.updated_at = NOW();
COMMIT;
```

관측 쿼리

```sql
SELECT
  NOW(6) AS captured_at,
  CAST((SELECT VARIABLE_VALUE
          FROM performance_schema.global_status
         WHERE VARIABLE_NAME = 'Innodb_history_list_length') AS UNSIGNED) AS innodb_history_list_length,
  (SELECT COUNT FROM information_schema.innodb_metrics WHERE name = 'trx_rseg_history_len') AS trx_rseg_history_len,
  (SELECT COUNT(*) FROM information_schema.innodb_trx) AS active_trx,
  IFNULL((
    SELECT MAX(TIMESTAMPDIFF(SECOND, trx_started, NOW(6)))
      FROM information_schema.innodb_trx
  ), 0) AS oldest_trx_age_s;
```
</details>

요약 결과:

| phase | max_innodb_history_list_length | max_trx_rseg_history_len |
|---|---:|---:|
| baseline (no long snapshot) | 0 | 37 |
| rr_snapshot (long RR tx held) | 0 | 522 |
| cooldown (after snapshot commit) | 0 | 548 |

> _phase: 측정 구간(baseline=긴 스냅샷 없음 / rr_snapshot=긴 RR 스냅샷 유지 / cooldown=커밋 후 purge가 따라잡는 구간)._
> _max_innodb_history_list_length: 구간 내 Innodb_history_list_length 최대값(샘플링 기반; 환경에 따라 NULL로 찍힐 수 있음)._
> _max_trx_rseg_history_len: 구간 내 trx_rseg_history_len 최대값(undo backlog/ purge 지연의 “MySQL스러운” 지표)._

해석:

- 우리 환경에서는 `Innodb_history_list_length`가 `NULL`로 찍혀서(그래서 표에 0으로 요약됨…🥲) 이 값은 참고만 했고,
- `trx_rseg_history_len`이 baseline 대비 rr_snapshot에서 확 커지는 걸 확인했습니다.

TSV 파일(시계열, 그래프용):

- `out/undo_history_*.tsv` (실험 실행 시 생성됨)

---

## 가설 C) SERIALIZABLE 운영비용: 대기/타임아웃/데드락

### 실험 C-1) 락 대기/데드락을 숫자로 뽑아보기(with performance_schema)

수치로 확인하고 싶어서 `performance_schema`를 같이 봤습니다.

이번 실험에서는 (orders 같은 대용량이 아니라) `bench_kv` 같은 작은 벤치 테이블로,락 대기/데드락을 격리수준 3종에서 재현해봤습니다.

_(벤치테이블: 성능 테스트에서 사용하는 테스트 테이블)_

| isolation | lockwait_waited_ms | deadlocks_delta | lock_wait_timeouts_delta |
|---|---:|---:|---:|
| READ COMMITTED | 4609.4670 | 1 | 0 |
| REPEATABLE READ | 4659.2530 | 1 | 0 |
| SERIALIZABLE | 4675.1040 | 1 | 0 |

> _isolation: 세션 격리수준._
> _lockwait_waited_ms: row lock wait에 “실제로 기다린 시간” 합계(ms, performance_schema 요약 기반)._
> _deadlocks_delta: 실험 전/후 데드락 카운트 증분._
> _lock_wait_timeouts_delta: 실험 전/후 lock wait timeout 카운트 증분._

- 격리수준이 곧바로 데드락을 만든다”라기보다,
  - **락을 잡는 순서**(lock ordering)
  - **범위/인덱스/트랜잭션 길이**
  가 더 직접적인 원인이 되는 경우가 많습니다.
- 그리고 SERIALIZABLE의 운영비용은 벤치테이블의 크기보다 실제 범위/실제 트랜잭션 길이에서 훨씬 잘 나타납니다. (다음 실험)

---

### 실험 C-2) 대용량 범위에서 SERIALIZABLE은 어떻게 터지나?

이번에는 꽤 큰 범위(`id 1..50,000,000`)에서,
배치가 범위를 읽고 있는 동안 OLTP가 같은 범위를 업데이트 하도록 만들었습니다.

### 결과(캡처)

배치 쪽은 스냅샷이 아주 안정적이었고(정합성은 좋음),

```text
BATCH_RANGE_DIFF  SERIALIZABLE  0  0
```

OLTP 업데이트는… 타임아웃이 났습니다..

```text
ERROR 1205 (HY000): Lock wait timeout exceeded; try restarting transaction
Exit codes: batch=0 oltp=1
```

### 해석(가설 C)

- SERIALIZABLE은 정합성은 강해지지만, 그 대가로 동시 쓰기(OLTP)가 **대기/타임아웃**으로 실패할 수 있어요.
- 특히 이번 환경은 `innodb_lock_wait_timeout=5`라서, 좀 기다리면 되지 않을까?” 하다가도 금방 1205가 뜹니다.
_ERROR 1205 (HY000): Lock wait timeout exceeded : 락 대기 시간초과_


SERIALIZABLE은
- **재시도 정책**
- **범위 축소**(작게/짧게)
- **워터마크/스냅샷 테이블** 같은 설계 패턴
- **충돌면적 줄이기**
를 다 고려해서 씁시다!

---

## 가설 D) 격리수준보다 “패턴”이 먼저다 — 중복/누락은 설계로 잡자

*_클레임 체크 패턴(Claim Check Pattern): 대용량 메시지를 큐에 직접 넣지 않고, 외부 저장소에 두고 토큰만 큐에 넣는 방식_

### 실험 D-1) 수동 재현: “나쁜 클레임” vs “좋은 클레임”

궁금증:

“READY 작업을 여러 배치가 동시에 집으면, 중복 처리가 생기나?”

나쁜 패턴(레이스 유발): `SELECT`로 그냥 읽고 → `SLEEP`(외부작업) → `INSERT` → DONE 업데이트

```text
ERROR 1062 (23000): Duplicate entry 'job-1' for key 'processed_log.PRIMARY'
```

좋은 패턴(제대로 된 클레임): `SELECT … FOR UPDATE SKIP LOCKED`

```text
A_CLAIMED  1  job-1
B_CLAIMED  2  job-2
```

<details><summary>sql 스크립트</summary>
나쁜 패턴
```sql
START TRANSACTION;
SELECT id, payload INTO @id, @payload
FROM bench_queue
WHERE state='READY'
ORDER BY id
LIMIT 1;
DO SLEEP(@work_sleep);
-- 이후 INSERT/UPDATE는 같음
COMMIT;
``` 
  
좋은 패턴
```sql
START TRANSACTION;
SELECT id, payload INTO @id, @payload
FROM bench_queue
WHERE state='READY'
ORDER BY id
LIMIT 1
FOR UPDATE SKIP LOCKED;
DO SLEEP(@work_sleep);
INSERT INTO bench_processed_log(dedupe_key, processed_at)
SELECT @payload, NOW() WHERE @id IS NOT NULL;
UPDATE bench_queue SET state='DONE' WHERE id=@id;
COMMIT;
```
</details>

요약하면:

- 격리수준을 뭐로 두든(물론 영향은 있지만), 클레임 패턴이 먼저다.
- “작업 선점(락) + SKIP LOCKED + 멱등키(unique)” 조합이 중요하다고 하는 데에는 다 이유가 있다!

---

### 실험 D-2) 큐 소비를 벤치로 돌려서 “중복 시도(1062)”를 숫자로 보기

실행 예시:

```bash
bash tools/run_queue_claim_bench_nodrop.sh good 8 20 0.05 "READ COMMITTED" 50000
bash tools/run_queue_claim_bench_nodrop.sh bad  8 20 0.05 "READ COMMITTED" 50000
```

결과(캡처 요약):

| mode | isolation | DONE | READY | done_per_sec | duplicate_errors(1062) | deadlocks |
|---|---|---:|---:|---:|---:|---:|
| good | READ COMMITTED | 876 | 49124 | 43.80 | 0 | 0 |
| bad | READ COMMITTED | 163 | 49837 | 8.15 | 728 | 0 |
| good | REPEATABLE READ | 849 | 49151 | 42.45 | 0 | 0 |
| bad | REPEATABLE READ | 150 | 49850 | 7.50 | 749 | 0 |
| good | SERIALIZABLE | 784 | 49216 | 39.20 | 0 | 0 |
| bad | SERIALIZABLE | 107 | 49893 | 5.35 | 0 | 733 |

> _mode: good=FOR UPDATE SKIP LOCKED로 선점(claim) / bad=SELECT 후 sleep으로 레이스 창 열기._
> _isolation: 워커 세션 격리수준._
> _DONE, READY: 벤치 종료 시점 bench_queue 상태별 남은 행 수._
> _done_per_sec: DONE/seconds(대략 처리량)._
> _duplicate_errors(1062): bench_processed_log unique 위반(1062) 발생 횟수(중복 처리 “시도”의 흔적)._
> _deadlocks: 워커 로그에서 텍스트 스캔으로 집계한 데드락 횟수(대략치)._

해석:

- good 패턴은 격리수준을 바꿔도 중복 시도(1062)가 0 → **패턴이 답!**
- bad 패턴은 RC/RR에서 중복 시도 폭발 + 처리량 급감
- bad+SERIALIZABLE은 1062가 줄어든 게 아니라, 데드락 비용으로 튄 케이스로 해석

---



## TL;DR

- **큐 소비는 격리수준보다 클레임 패턴이 먼저다**  
  `FOR UPDATE SKIP LOCKED + dedupe unique`면 중복 시도 자체를 크게 줄일 수 있음. (bad 패턴을 SERIALIZABLE로 “덮기”는 오히려 데드락/처리량 문제로 돌아올 수 있음)
- **RC 집계는 배치 1회 안에서도 값이 흔들릴 수 있다**  
  “같은 트랜잭션인데 합계가 달라요”가 실제로 재현됨.
- **RR은 읽기 일관성은 좋지만, 범위/장시간에서는 경합 비용을 꼭 봐야 한다**  
  gap/next-key 벤치에서 RC(≈11ms) vs RR/SERIALIZABLE(≈4.6s)로 “줄 서는 시간”이 확 커지는 걸 확인.
- **SERIALIZABLE은 정합성은 강하지만, 운영비용이 진짜로 튄다**  
  대용량 범위 스윕에서 50M 구간은 OLTP가 `ERROR 1205`로 실패하는 모습이 관측됨.
- **대용량에서 트레이드오프가 훨씬 선명해진다**  
  range가 커질수록 RC는 `batch_sum_diff`가 커지고, SERIALIZABLE은 “정합성은 안정적이지만” OLTP가 타임아웃/실패로 갈 수 있음.
- **RR 장시간 스냅샷은 undo backlog를 키울 수 있다**  
  `trx_rseg_history_len`가 baseline(최대 37) -> rr_snapshot(최대 522)로 확 커지는 걸 확인.
- **chunk commit은 격리수준과 별개로 스냅샷을 깨뜨릴 수 있다**  
  chunk1/2가 업데이트 전/후를 따로 읽으면 mixed snapshot 오차가 난다.
