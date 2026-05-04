# MVCC

### 잠금

#### X-Lock (#배타락, #쓰기락)

- Update, Delete, Insert,Select for Update 시 사용
- 이 잠금이 걸린 데이터는 다른 어떤 트랜잭션도 읽기 잠금(S-Lock)이나 쓰기 잠금(X-Lock)을 걸 수 없다.
- 트랜잭션이 커밋 되거나 롤백 될 때 까지 유지된다.

#### S-Lock (#읽기락, #공유락)

- Select 시 사용
- 이 잠금이 걸린 데이터는 다른 트랜잭션이 또 읽을 수 있다. → 여러명이 동시에 S-Lock 가능
- 하지만 다른 트랜잭션이 쓰기(X-Lock)는 불가능

## 트랜잭션이란

-all or nothing

트랜잭션은 작업의 완전성을 보장해주는 성질. 즉, 여러 논리적인 작업이 있을 때, 이를 하나의 집합으로 묶어서 처리하는 개념이다. 모두 완벽하게 처리하거나, 하나라도 실패하면 모두 작업 처리 전 상태로 되돌 릴 수 있어야 함. 사용자 입장에서는 작업의 논리적인 단위로 이해할 수 있고, 시스템 입장에서는 데이터들을 접근하고 변경하는 단위로 다룰 수 있다.

### 트랜잭션 설정 범위

트랜잭션은 꼭 필요한 최소의 범위에 적용하는 것이 좋음.

DB 의 커넥션 수가 제한적이고, 커넥션을 소유하는 시간이 길어질수록 여유 커넥션을 줄어들고 모든 커넥션이 소진되었다면 교착 상황이 일어날 수 있으며 병렬 처리의 성능이 줄어들기 때문

## ACID

트랜잭션이 가져야 할 4가지 속성. 원자성, 일관성, 격리성, 지속성이 있음.

- Atomicity, 원자성: 트랜잭션 중에 아무런 문제도 발생하지 않은 경우에만 모든 작업들이 처리되어야 한다. 어떤 트랜잭션 중간에 문제가 발생하면 해당 트랜잭션 내의 어떤 작업도 수행되서는 안된다. 이미 수행됐다면 수행 전으로 되돌려야 한다.
- Consistency, 일관성: 트랜잭션이 완료된 상태 이후에도, 트랜잭션이 처리되기 전과 같게 데이터의 일관성을 보장해야 한다. 예를 들어, 스키마/제약조건/비즈니스 규칙 등이 바뀌어선 안된다.
- Isolation, 격리성: 각각의 트랜잭션은 서로 간섭없이 독립적으로 수행되어야 한다. 동시에 실행되더라도 마치 순차적으로 실행된 것과 같아야 한다. (락, 트랜잭션 격리수준을 통해 구현한다.)
- Durability, 지속성: 커밋이 안료되면 영구적으로 데이터베이스에 작업의 결과가 저장되어야 한다. 꺼짐/장애 등이 발생하더라도 이를 보장해야 한다.

## rollback

트랜잭션 수행 중 변경사항을 취소하고 변경 전으로 되돌리는 것. 원자성을 지키기 위한 작업.

mysql의 경우, undo 로그를 기반으로 되감는다. postgre의 경우 버전 정보를 이용한다.

### undo와 redo in MySQL, PostgreSQL

MySQL

- undo: 롤백, MVCC에 사용
- redo: 커밋된 데이터를 나중에 적용할 때 씀 데이터 정합성 틀어지면 재적용으로 복구시킴

PostgreSQL

- 전통적인 의미에 undo가 없음…
- MVCC를 row새로 쓴 버전을 둠

## 로그 시스템(InnoDB 기준)

### Redo Log

“이 변경을 다시 수행하라”는 변경 기록 로그

→ Crash 이후 복구를 위한 로그

**why?**

DB는 성능을 위해

- 데이터를 바로 디스크에 쓰지 않고
- Buffer Pool(메모리) 에 먼저 반영한다.

**but**

서버가 갑자기 다운되면
→ 메모리에 있던 변경 내용이 사라짐

**so**

1. 변경 내용을 Redo Log에 먼저 기록
2. 그 다음 메모리에 반영
3. 나중에 디스크에 flush

**동작 흐름**

트랜잭션이 UPDATE 실행하면

1. Redo Log Buffer에 기록
2. commit 시 Redo Log File에 flush
3. 이후 데이터 페이지를 디스크에 반영

Crash 발생 시

- Redo Log를 읽어
- 반영되지 않은 변경을 재적용 (roll forward)

### Redo Log가 보장하는 것

- Durability
- WAL 기반 안정성

**특징**

- 순차 기록 (Sequential Write → 빠름)
- 변경 결과를 저장
- Rollback에는 사용하지 않음

### Undo Log

“이 변경을 되돌려라”는 이전 상태 기록 로그

- Rollback용
- MVCC용

**why?**

**상황 Rollback** 

`UPDATE user SET balance=0;`→ 오류 발생 → rollback

이전 값이 필요함→ Undo Log에 이전 값 저장되어 있음

**상황 MVCC** 

Repeatable Read에서

- 내가 트랜잭션 시작했을 때
- 다른 트랜잭션이 UPDATE

과거 버전의 데이터를 불러와야하는데 Undo Log에 저장된 이전 버전을 참조

### MySQL에서의 rollback

1. 쓰기 tx발생(update/delete/insert), 락 획득
2. undo로그에 tx전 정보 기록
3. 버퍼풀의 페이지를 실제로 수정
4. redo에 변경사항을 기록

rollback이 호출되면

- 이 트래잭션이 만든 undo레코드를 최신 것부터 역순으로 적용(stack)
- 롤백도 페이지를 수정하는것이므로 redo에 기록함

주의사항

- SAVEPOINT는 undo 로그의 특정 시점을 찍어두고 그 이후만 부분 롤백.
- 대량 변경 후 롤백은 undo 적용 자체가 오래 걸릴 수 있고, 그동안 락 때문에 다른 트랜잭션이 막힐 수 있다.

### PostgreSQL에서의 rollback

postgreSQL에서의 MVCC는 기존 row를 덮어쓰지 않음

- update: 새로운 버전의 튜플을 만들고 이전 버전을 남김.
- delete: tuple를 물리적으로 삭제하지 않고, 삭제됐다고 표시해둔다.

> tuple: 한 record의 한 버전

tuple에 붙는 메타: xmin, xmax

- xmin: 튜플을 만든 tx-id
- xmax: 튜플을 죽인(갱신/삭제) tx-id

tx1 rollback 호출 시(abort)

- 트랜잭션 상태 테이블(commit/abort 여부 기록)에 tx1을 abort로 기록
- tx1이 만든 새 튜플은 커밋되지 않은 버전이므로 다른 트랜잭션에서 보이지 않음
- 별도의 undo를 길게하지 않고도 논리적으로 변경이 사라짐

물리적으로는..

- abort된 트랜잭션이 만든 튜플/업데이트로 인한 쓰레기 튜플은 디스크에 남을 수 있음
- 이건 vaccum이 회수해서 공간을 재사용 가능하게 만든다.

주의사항

- 롤백이 빠르다?
    undo를 적용하며 되감는 작업이 적다. 
    but, 롤백 시점까지 잡고 있던 락은 트랜잭션 종료까지 유지되는 경우가 많아서, 동시성 측면에서 체감 영향은 여전히 클 수 있다.

## 트랜잭션 격리 수준

여러 트랜잭션이 동시에 처리될 때, 특정 트랜잭션이 다른 트랜잭션에서 변경하거나 조회하는 데이터를 볼 수 있게 허용할지 여부를 결정하는 것

### 격리 수준의 종류

| 격리수준\이상현상 | DIRTY READ | NON-REPEATABLE READ | PHANTOM READ |
| --- | --- | --- | --- |
| READ UNCOMMITTED | O | O | O |
| READ COMMITTED | X | O | O |
| REPEATABLE READ | X | X | O (InnoDB는 X) by Next Key Lock |
| SERIALIZABLE | X | X | X |

4가지의 격리 수준에서 아래로 갈 수록 격리 수준이 높아지고 동시 처리 성능이 떨어진다.

READ UNCOMMITTED는 거의 사용X, SERIALIZABLE는 동시성이 중요한 경우엔 거의 사용하지 않는다.

### 세 가지 이상현상

1. Dirty Read (더티 리드)

![img](/assets/dirty_read.png)

- 다른 트랜잭션이 아직 커밋하지 않은 값을 읽는 것
- 상대가 롤백하면 존재하지 않았던 값을 읽은 것.

1. Non-Repeatable Read

![img](/assets/unrepeated_read.png)

- 같은 트랜잭션에서 같은 행을 두 번 읽었는데 값이 달라지는 것
- 중간에 다른 트랜잭션이 커밋한 업데이트가 반영됨

1. Phantom Read

![img](/assets/phantom_read.png)

- 같은 트랜잭션에서 같은 조건으로 여러 행을 조회했는데, 두 번째 조회에서 행의 개수/구성이 바뀌는 것
- 중간에 다른 트랜잭션이 조건에 맞는 행을 INSERT/DELETE해서 발생

## Serializable

트랜잭션을 순차적으로 진행시킴.

- 어떤 부정합 문제도 발생하지 않음. 다만 모든 트랜잭션이 순차적으로 처리되어 동시처리 성능이 매우 떨어짐

### **MySQL(InnoDB) SERIALIZABLE**

비관락 느낌

- InnoDB는 SERIALIZABLE에서 순수 SELECT를 공유락 읽기로 수행
- 읽기도 락을 잡으니까, 동시성이 크게 떨어지고 대기가 늘 수 있음.
- 범위 조회: locking read가 인덱스 범위를 잠금 → phantom을 락으로 막음
- 블로킹/데드락/락 대기가 늘어남

**Non Locking Consistent Read**

InnoDB 는 MVCC 를 활용하여 Lock 을 사용하지 않고 읽기 작업을 수행함.

- 격리 수준이 serializable 이 아니라면 SELECT 의 경우 다른 트랜잭션과 별개로 Lock 을 기다리지 않음. 이것을 Non Locking Consistent Read 라고 함
⇒ InooDB 에서는 Undo 영역을 사용
- 트랜잭션이 너무 오래 활성화 되어 있다면, 서버가 느려지거나 문제가 발생할 수 있으므로, 트랜잭션이 실행되었다면 커밋이나 롤백을 빨리 하는 게 좋음

### **PostgreSQL SERIALIZABLE**

SSI(Serializable Snapshot Isolation) 방식

낙관락 느낌

- 스냅샷으로 읽으면서, 동시 트랜잭션 간 read/write 의존성을 감시해서 직렬 실행으로 불가능한 패턴이면 한 쪽을 롤백시킴
  - 락으로 다 막기보단 실패 + 재시도
  - 애플리케이션 레벨에서 같은 트랜잭션 블록 재시도가 필수적임

## Reapetable Read

- Dirty read 방지
- Non-repeatable read 방지(같은 행을 두 번 읽으면 값이 바뀌지 않게)

동작 원리

- 읽은 행에 대해 공유락(읽기락) 걸고 트랜잭션을 유지
    → 다른 트랜잭션의 쓰기락(배타락)이 막혀서 같은 행의 값은 반복해서 동일하게 보임.
- 하지만 범위 조건에서 범위 전체를 잠그는 범위락은 보장하지 않아서, 다른 트랜잭션이 그 조건에 들어오는 새 행을 INSERT하면 두 번째 조회에서 행 집합이 바뀌는 phantom이 생길 수 있음.

### **MySQL(InnoDB) RR**

InnoDB는 RR에서 스냅샷(순수 SELECT)과 락을 둘 다 씀.

일반 SELECT(락 없는 순수 SELECT) = Undo 스냅샷

- InnoDB RR에서 SELECT는 락을 안 걸고, 첫 read에서 만든 스냅샷을 트랜잭션 끝까지 유지함.
- 그래서 TX2가 중간에 INSERT를 커밋해도 TX1의 스냅샷엔 안 보이니까, phantom이 안 보임

락 읽기/쓰기 쿼리: 락, next-key/gap락 로 범위 삽입 차단

- RR에서 (select for update, update, delete)는 조건에 따라 락이 달라짐
  - 유니크 인덱스 + 유니크 조건이면 레코드 락만
  - 나머지(범위/비유니크)스캔한 인덱스 범위를 next-key/gap lock으로 잠가서 그 범위에 다른 세션이 INSERT 못 하게 막음  

Gap lock: 인덱스 레코드와 레코드 사이의 구간(틈) 을 잠금
> 이 구간에 해당하는 키 범위로 새 레코드 INSERT를 못하게 막음

Next key lock: 레코드 r과 r앞에 범위 gap을 다 락거는거
> Next-key lock은 범위에 update도 막힘. gap lock은 update는 안막힘(인덱스 키 제외)

주의 사항

- 공식문서: RR에서 락(UPDATE/INSERT/DELETE/SELECT … FOR …)와 순수 SELECT를 섞지 말라함.

- 순수 SELECT은 과거, locking은 최신 상태라 같은 트랜잭션 안에서 서로 다른 시점을 보게돼서 헷갈림

### Snapshot Read vs Current Read

MySQL InnoDB의 RR에서는 읽기 방식이 두 가지로 나뉩니다.

1. **Consistent Read (Snapshot Read):**
    - 일반적인 `SELECT` 문.
    - 트랜잭션이 시작될 때 찍어둔 **스냅샷(과거 버전)**을 봅니다.
    - 따라서 다른 트랜잭션이 `COMMIT`을 해도 내 눈에는 안 보입니다. (Phantom Read 방지됨)
2. **Locking Read (Current Read):**
    - `INSERT`, `UPDATE`, `DELETE`, `SELECT ... FOR UPDATE` 문.
    - 데이터를 수정하려면 과거 사진이 아니라 **"현재의 최신 실제 데이터"**를 보고 락을 걸어야 합니다.
    - **여기서 스냅샷을 무시하고 현재 데이터를 조회하게 됩니다.**

### **PostgreSQL RR**

Snapshot Isolation: 트랜잭션 시작 시점의 스냅샷만 봄

- tx2가 중간에 INSERT/DELETE를 커밋해도, tx1이 같은 조건으로 다시 SELECT하면 행 집합이 바뀌지 않음

업데이트 충돌

- RR에서 `UPDATE/DELETE/SELECT FOR UPDATE` 등 락 작업은 트랜잭션 시작 스냅샷을 기준으로 대상 행을 찾음
- 행이 시작 이후 다른 트랜잭션에 의해 바뀌어 커밋됐다면 대기
- 결국 RR 트랜잭션은 에러나면서 롤백되고 재시도가 필요함

## Read Committed

→ non reapeatable read, phantom read

한 트랜잭션 내에서 여러번 조회할 경우 다른 트랜잭션의 커밋여부에 따라 다른 결과가 나올 수 있다.

보장: Dirty read 방지(커밋된 것만 읽음)

허용: Non-repeatable read / Phantom read 가능

직관: 각 쿼리마다 최신 커밋 상태를 본다 → 같은 tx안에서 여러번 조회하면 결과가 바뀔 수 있음

예를 들어, 어떤 트랜잭션에서는 오늘 입금된 총 합을 계산하고 있는데, 다른 트랜잭션에서 계속해서 입금 내역을 커밋하는 상황이라고 하자. 그러면 READ COMMITTED에서는 같은 트랜잭션일지라도 조회할 때마다 입금된 내역이 달라지므로 문제가 생길 수 있다.

### mySQL(InnoDB) RC

- 읽기(순수 SELECT)
  - 각 SELECT마다 새 스냅샷을 잡고 커밋된 데이터만 본다.
  - 같은 트랜잭션에서도 두 번 SELECT하면 결과가 바뀔 수 있음(Non-repeatable/phantom 등).
- 락 전략
  - RC로 바꾸면 검색/인덱스 스캔에서 gap lock이 꺼짐: 즉 범위에 대한 삽입 차단(gap)이 약해져서 동시 INSERT가 잘됨
  - 예외: 외래키 체크, 중복키 체크(유니크 충돌) 같은 일부 상황에서는 갭 락이 쓰일 수 있음.

결과적으로..

- RC + `SELECT ... FOR UPDATE`를 해도 읽은 행은 잠그지만, 범위의 틈 삽입까지 막는 건 RR보다 약함.
- 범위 조건 기반 로직은 유니크 제약 + 재시도 패턴이 안전

### PostgreSQL RC

- 읽기(순수 SELECT)
  - 각 쿼리 시작 시점 스냅샷을 본다. 각 쿼리는 시작 전에 커밋된 row만 볼 수 있음.
  - 같은 트랜잭션 안에서도 다음 SELECT은 더 최신 커밋을 볼 수 있음 → Non-repeatable/phantom
- 락 읽기/쓰기
  - `SELECT ... FOR UPDATE/UPDATE/DELETE`는 행 단위로 잠그고 경쟁하면 대기하는 방식
  - 범위 삽입을 막아야 하면 보통 대표행을 잠그거나, 유니크 제약 + 재시도로 풀어야함

## Read Uncommitted

→ dirty read, non reapeatable read, phantom read

READ UNCOMMITTED는 커밋하지 않은 데이터 조차도 접근할 수 있는 격리 수준이다.

READ UNCOMMITTED에서는 다른 트랜잭션의 작업이 커밋 또는 롤백되지 않아도 즉시 보이게 된다.

어떤 트랜잭션의 작업이 완료되지 않았는데도, 다른 트랜잭션에서 볼 수 있는 부정합 문제를 Dirty Read(오손 읽기)라고 한다

→ postgres는 read uncommited도 read committed처럼 동작

# InnoDB Storage Engine Lock

InnoDB 는 MySQL 에서 제공하는 잠금과는 별개로 레코드 기반의 잠금 방식을 탑재함
⇒ 동시성 처리가 매우 뛰어남

### Record Lock

레코드 자체만을 잠금. InnoDB 는 레코드 객체가 아닌 인덱스의 레코드를 잠금
인덱스 생성을 안했어도 자동 생성되는 클러스터 인덱스를 이용하여 잠금

### Gap Lock

레코드와 바로 인접한 레코드 사이의 간격만을 잠그는 것.

레코드와 레코드 사이의 간격에 새로운 레코드가 생성되는 것을 제어할 수 있음

갭 락 자체보단 넥스트 키 락의 일부로 주로 사용됨

### Next Key Lock

레코드 락과 갭 락을 합친 락.

바이너리 로그에 기록되는 쿼리가 레플리카 서버에서 실행될 때 소스 서버에서 만들어 낸 결과와 동일한 결과를 만들어내도록 보장함

### Auto Increment Lock

AUTO_INCREMENT 컬럼이 사용된 테이블에 여러 레코드가 INSERT 될 경우, 각 레코드는 중복되지 않고 순차적인 값을 가져야하는데, 이를 위해 자동 증가 락을 사용함.

시스템 변수를 통해 서버가 INSERT 되는 레코드 건 수를 정확히 예측할 수 있을 때는 자동 증가 락 대신 뮤텍스를 이용하여 처리할 수 있음.

## MySQL Engine Lock

### Global Lock

한 세션에서 글로벌 락을 획득한다면 다른 세션에서 SELECT 를 제외한 대부분의 DDL, DML 문장을 실행하는 경우 글로벌 락이 해제될 때까지 대기 상태로 남음

범위가 MySQL 서버 전체이므로 테이블이나 데이터베이스가 다르더라도 영향을 받는다.

전체 데이터의 변경 작업을 멈추어야할 때 (백업 또는 마이그레이션) 적용할 수 있겠지만, 트랜잭션을 지원하므로 그럴 필요가 없고, 글로벌 락은 서버에 큰 영향을 미치므로 사용하지 않는 것이 좋다.

### Table Lock

개별 테이블 단위로 설정되는 락

- 명시적 테이블 락 : 글로벌 락과 마찬가지로 서버에 큰 영향을 미치므로 사용할 필요가 거의 없음
- 묵시적 테이블 락 : InnoDB 는 스토리지 엔진 차원에서 레코드 기반 잠금을 제공하므로 스키마를 변경하는 DDL 쿼리의 경우에만 테이블 락이 설정됨

### Named Lock

사용자가 지정한 문자열에 대해 잠금을 설정 (”name”)

자주 사용되지는 않고, 여러 클라이언트가 상호 동기화를 처리해야 하는 경우에 사용됨

ex) 1대의 DB 서버에 여러 서비스가 접속하는 서비스에서 여러 웹 서버가 같은 정보를 동기화 하는 경우

예시와 같이 여러 서비스가 접속해 있는 상황에서 한번에 많은 데이터를 수정하는 경우 데드락의 원인이 됨.

⇒ 이 때 동일한 데이터를 변경하거나 참조하는 프로그램끼리 분류해서 네임드 락을 걸면 데드락을 막을 수 있음

자세히 설명해줘

> 뮤텍스(Mutex)"와 같은 원리
>
> - 특정 코드 블록을 실행하기 위해 키(Key)를 얻어야 하죠?
> - 네임드 락은 그 **키의 이름을 문자열로 지정**하는 것입니다.
>
> 왜 문자열(이름)로 할까?
>
> 실제 데이터가 아닌 문자열을 사용하는 이유는 **"존재하지 않는 상태"**를 제어하기 위해서입니다.
>
> - **상황:** 사용자 `user_100`이 동시에 회원가입 버튼을 두 번 눌렀습니다.
> - **문제:** DB에 아직 데이터가 없으므로 `WHERE id=100` 같은 레코드 락을 걸 대상이 없습니다.
> - **해결:** `SELECT GET_LOCK('register_user_100', 5)`를 실행합니다.
>   - 첫 번째 요청이 `'register_user_100'`이라는 문자열로 락을 획득합니다.
>   - 두 번째 요청은 똑같은 문자열로 락을 요청하지만, 이미 선점되었으므로 대기하게 됩니다.

### MetaData Lock

DB 객체(테이블, 뷰 등)의 이름이나 구조를 변경하는 경우에 획득

명시적으로는 획득이 불가능하고, 테이블의 이름을 변경할 때 자동으로 획득.
