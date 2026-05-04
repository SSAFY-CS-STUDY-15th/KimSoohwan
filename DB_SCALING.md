# 이중화

## Replication

Replication이란 무엇인가요?

- 하나의 DB 데이터를 다른 DB 서버에 복제해 동일하거나 유사한 데이터 사본을 유지하는 구조
- 주로 고가용성, 장애 복구, 읽기 부하 분산을 위해 사용함
- 원본 서버의 변경 사항을 복제 서버로 전달해 데이터 동기화를 수행함
Master-Slave (Primary-Replica) 구조를 설명해보세요.
- Master 가 쓰기 요청을 처리하고, Slave 가 Master 의 데이터를 복제해 보관하는 구조
- Master 에서 발생한 INSERT, UPDATE, DELETE 변경 사항을 Replica로 전파함
- Slave 는 주로 읽기 요청 처리나 백업, 장애 대응 용도로 사용함
- Master 장애 시 Slave 를 승격해 서비스 지속 가능
Master-Slave (Primary-Replica) 구조에서 쓰기와 읽기는 어떻게 분리하나요?
- 쓰기 요청은 데이터 정합성을 위해 Master DB로 전달하는 방식
- 읽기 요청은 부하 분산을 위해 Slave DB로 분산하는 방식
- 애플리케이션 또는 DB 프록시 계층에서 쿼리 유형에 따라 라우팅함
- 단, 비동기 복제 환경에서는 Replica 지연으로 최신 데이터 조회 불일치 가능성 존재

## Sharding

Sharding과 Partitioning의 차이는 무엇인가요?

- partitioning: 하나의 DB 내부에서 테이블 또는 데이터를 나누는 방식
  - 관리/조회 성능 개선 목적
  - 같은 DB 인스턴스 내부 분할
- sharding: 데이터를 여러 DB 서버 또는 인스턴스에 분산 저장하는 방식
  - 수평 확장 목적
  - 저장 공간, 트래픽, 부하 분산
  - 애플리케이션 또는 미들웨어의 샤드 라우팅 필요

Horizontal Partitioning과 Vertical Partitioning의 차이는 무엇인가요?

- Horizontal Partitioning
  - 행 기준 분할
  - 같은 스키마의 데이터를 여러 구간으로 분리
  - 예: user_id 1-10000, 10001-20000
  - 데이터 양 증가에 대응
- vertical Partitioning
  - 컬럼 기준 분할
  - 자주 쓰는 컬럼과 덜 쓰는 컬럼 분리
  - row 크기 감소, 조회 효율 개선

샤딩 방법은 무엇이 있나요?

- Range Sharding, Hash Sharding, 등등..

Range Sharding의 장단점은 무엇인가요?

- 장점
  - 구현 단순, 범위 조회에 유리
  - 날짜, ID 구간 기반 조회에 적합
- 단점
  - 특정 구간 트래픽 집중 가능성
  - Hot Shard 발생 위험
    Hash Sharding의 장단점은 무엇인가요?
- 장점
  - 데이터 분산 균등
  - 특정 샤드 집중 완화
- 단점
  - 범위 조회에 불리
  - 정렬/기간 조회 어려움
  - 샤드 추가 시 재배치 비용 발생
  - 특정 조건 조회 시 여러 샤드 조회 필요 가능성

## CAP

> CAP 정리에 따르면 분산 시스템은 일관성(consistency), 가용성(availability), 및 파티션 허용(partition tolerance)(CAP의 'C', 'A', 'P')이라는 세 가지 특성 중 두 가지 특성만 제공할 수 있습니다.

Consistency 일관성(금융)

- 모든 노드에서 같은 시점에 동일한 데이터 조회 보장
- 쓰기 성공 이후 모든 읽기에서 최신 값 조회
- 데이터 정합성 우선

Availability 가용성(SNS)

- 모든 요청에 대해 정상 응답 보장
- 일부 노드 장애 상황에서도 서비스 응답 유지
- 최신 데이터가 아닐 수는 있음
- 응답 가능성 우선

Partition Tolerance

- 네트워크 단절 또는 노드 간 통신 실패 상황에서도 시스템 동작 유지
- 분산 시스템에서 사실상 포기하기 어려운 특성
- 네트워크 장애를 전제로 한 설계 필요
- CAP에서 현실적으로 CP/AP 선택의 기준

네트워크 파티션 상황에서 CP와 AP 시스템은 어떻게 다르게 동작하나요?

- CP 시스템 : Consistency + Partition Tolerance `금융`
  - 네트워크 파티션 상황에서 정합성 우선
  - 최신 상태를 보장할 수 없는 요청은 실패 또는 대기 처리
  - 일부 가용성 포기
  - 잘못된 데이터 처리 방지 목적

- AP 시스템: Availability + Partition Tolerance `SNS`
  - 네트워크 파티션 상황에서도 응답 우선
  - 일시적으로 노드 간 데이터 불일치 허용
  - 이후 동기화로 최종 일관성 보장
  - 일부 정합성 지연 감수
