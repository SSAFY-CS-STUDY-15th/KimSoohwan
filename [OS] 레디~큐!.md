# 레디~~ 큐!(숙취해소제 아님)

## 들어가며

다량의 요청을 처리할때, 성능은 큐 자료구조 하나만으로 결정되지 않습니다. 락을 어떻게 잡는지, producer와 consumer가 몇 개인지, queue가 하나인지 여러 개인지, 큐가 꽉 찼을 때 기다리는지 버리는지, dispatcher를 거치는지 바로 worker에게 가는지까지 다 확인해야 합니다.

오늘 보려고 하는 것

- 하나의 shared queue를 유지해야 한다면 어떤 락 전략이 현실적인가?
- SPSC처럼 좁은 계약을 받아들일 수 있다면 시스템 구조를 어떻게 바꿔야 하는가?
- dispatcher를 두는 구조와 바로 worker-local queue로 넣는 구조는 무엇이 다를까?
- TPS, p99, drop, imbalance 같은 지표는 어떤 조건에서 어떻게 읽어야 하는가?

C++로 구현한 여러 queue와 pipeline benchmark를 바탕으로 어떤 구조가 효율적인지 정리해봤습니다.

---

## 사전 지식

### Producer와 Consumer

큐를 이해하려면 먼저 producer와 consumer를 알고갑시다.

producer는 일을 만들어 큐에 넣고, consumer는 큐에서 일을 꺼내 처리합니다.

예를 들어 웹 서버를 생각해 보자.

- 요청을 받아 작업 객체를 만드는 thread는 producer에 가깝다.
- 큐에서 작업을 꺼내 실제로 처리하는 worker thread는 consumer에 가깝다.

큐는 둘 사이의 완충지대(?)라고 생각하면 됩니다.

producer가 잠깐 빠르게 일을 넣어도 consumer가 순서대로 처리할 수 있게 해줍니다. 
반대로 consumer가 잠깐 느려져도 producer가 바로 멈추지 않게 해줍니다.

하지만 여러 thread가 동시에 큐를 건들면, 큐는 공유 자원이 됩니다. 여기서부터 락, 경합, tail latency 같은 친구들이 등장합니다.

### Shared Queue와 Local Queue

큐 구조를 크게 두 가지로 나눠 보겠습니다.

1. shared queue

```text
Producer 1 ┐
Producer 2 ├──> Shared Queue ───> Worker 1
Producer 3 ┘                    └> Worker 2
                                └> Worker 3
```

여러 producer와 여러 consumer가 하나의 큐를 같이 쓰는 구조입니다.

일단 단순하다는 장점이 있습니다. 중앙 큐 하나만 보면 되걸랑요. 작업을 어느 worker에게 보낼지 복잡하게 고민하지 않아도 되고, worker들은 그냥 같은 큐에서 하나씩 꺼내면 됩니다.

근데, 경합이라는 단점이 있어요. 모두가 같은 큐에 접근하므로 thread가 많아질수록 큐가 힘들어해요..

2. local queue

```text
Producer/Ingress ──> Worker 1 Queue ──> Worker 1
Producer/Ingress ──> Worker 2 Queue ──> Worker 2
Producer/Ingress ──> Worker 3 Queue ──> Worker 3
```

worker마다 자기 큐를 따로 갖는 구조입니다.

장점은 경합 분산입니다. 각 worker가 자기 큐만 보면 되므로, 하나의 큐에 모든 thread가 몰리는 상황을 줄일 수 있습니다.

단점은 routing입니다. 어떤 작업을 어느 worker queue에 넣을지 결정해야 합니다. 
잘못 나누면 특정 worker queue만 과열되고 나머지는 놀 수 있습니다. 로드 밸런싱이 필요해요.

### Topology란

여기서 topology는 “작업이 어떤 경로로 이동하는가”를 뜻합니다.

같은 SPSC queue(1 producer, 1 consumer)를 쓰더라도 아래 두 구조는 다릅니다.

```text
Ingress ──> Dispatch Queue ──> Dispatcher ──> Worker-local SPSC ──> Worker
```
첫 번째 구조는 dispatcher를 한 번 거친다. 중앙에서 작업을 나눠 줄 수 있으므로 균형을 잡기 좋습니다. 대신 queue hop이 하나 늘어납니다.

```text
Ingress ──> Worker-local SPSC ──> Worker
```


두 번째 구조는 ingress가 바로 worker-local queue에 넣습니다. 경로가 짧은 대신 라우팅이 잘못되면 특정 worker만 뜨거워질 수 있습니다.

### Contention, 경합

contention은 여러 thread가 같은 자원을 동시에 사용하려고 부딪히는 현상입니다.

가장 쉬운 예시는 mutex다.

```cpp
std::mutex mu;

void f() {
  std::lock_guard<std::mutex> lock(mu);
  // shared data 접근
}
```

한 thread가 `mu`를 잡고 있으면 다른 thread는 기다려야 겠죠? thread가 두세 개면 괜찮아 보일 수 있지만, producer 8개, consumer 8개가 모두 같은 mutex를 잡으려고 하면?? -> 경합이 발생합니다

락 자체의 비용도 있지만, 더 큰 문제는 대기열입니다. 누가 먼저 락을 잡는지, 얼마나 오래 잡는지, 언제 깨어나는지에 따라 지연이 뒤죽박죽 됩니다. 이게 tail latency를 키웁니다.

### Tail Latency와 p99

평균 latency는 전체 요청의 평균 지연입니다. 하지만 tail latency를 중요하게 보는 이유가 있습니다.

예를 들어 100개 요청 중 99개는 10ms에 끝나고, 1개는 1s가 걸렸다고 해봅시다. 평균만 보면 나쁘지 않아 보일 수 있습니다. 하지만 실제 운영에서는 그 1개의 튀는 지연이 결함이 됩니다.

p99는 100개 중 99번째로 느린 요청의 지연이다. 즉 “대부분은 이 안에 들어온다”를 보는 지표입니다.

큐 벤치마크에서 p99는 특히 중요합니다. 특히 block 모드에서는 더 그렇다. block은 큐가 꽉 찼을 때 기다리는 정책이기 때문에, 시스템이 약해지면 평균보다 p99가 먼저 튑니다.

### Backpressure: Block과 Drop

큐가 꽉 찼을 때 시스템은 둘 중 하나를 선택해야 합니다.

첫 번째는 block입니다.

```text
큐가 꽉 참 -> producer가 기다림 -> 빈자리가 생기면 다시 넣음
```

작업을 버리지 않는 대신 지연이 늘어납니다.

두 번째는 drop입니다.

```text
큐가 꽉 참 -> 새 작업을 받지 않음 -> 실패 또는 손실로 처리
```

지연 폭발은 줄일 수 있지만 작업을 버립니다.


---

## Global Lock Queue

가장 직관적인 큐 구현은 큐 전체를 mutex 하나로 보호하는 방식입니다.

이 구조를 `global_lock`이라고 부르겠습니다.

- push할 때 mutex를 잡는다.
- pop할 때도 같은 mutex를 잡는다.
- 한 번에 하나의 thread만 큐를 건들 수 있다.

```cpp
bool push(Event ev, BackpressurePolicy policy,
          const std::atomic<bool>& stop_flag) override {
  std::unique_lock<std::mutex> lock(mu_);

  if (capacity_ != 0 && queue_.size() >= capacity_) {
    if (policy == BackpressurePolicy::Drop) {
      return false;
    }

    not_full_cv_.wait(lock, [&] {
      return closed_ ||
             stop_flag.load(std::memory_order_relaxed) ||
             queue_.size() < capacity_;
    });
  }

  queue_.push_back(std::move(ev));
  not_empty_cv_.notify_one();
  return true;
}

bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
  std::unique_lock<std::mutex> lock(mu_);

  not_empty_cv_.wait(lock, [&] {
    return closed_ ||
           stop_flag.load(std::memory_order_relaxed) ||
           !queue_.empty();
  });

  if (queue_.empty()) return false;

  out = std::move(queue_.front());
  queue_.pop_front();
  not_full_cv_.notify_one();
  return true;
}

private:
  std::mutex mu_;
  std::condition_variable not_empty_cv_;
  std::condition_variable not_full_cv_;
  std::deque<Event> queue_;
```

이 코드에서 핵심은

```cpp
std::unique_lock<std::mutex> lock(mu_);
```

push도 같은 `mu_`를 잡고, pop도 같은 `mu_`를 잡습니다.

이게 global lock의 본질입니다.

### 장점

일단 단순합니다.

구현하기 쉽고, correctness를 설명하기 쉽습니다. 큐 내부 상태가 이상해질 가능성도 낮습니다. 

처음 동시성 큐를 만들 때는 이런 구조가 좋은 출발점이고, baseline으로도 좋습니다.

### 단점

문제는 병렬성이 커지면?

producer와 consumer가 모두 같은 mutex에서 만나니까. producer끼리도 충돌하고, consumer끼리도 충돌하고, producer와 consumer도 충돌합니다.

즉 병목이 한 지점에 몰립니다.

실험에서도 이 특성이 보였는데..

- primitive 수준 `ceiling_drop` 기준으로 TPS 중앙값은 약 `1.67M`, p99 중앙값은 약 `212.50us`였습니다.

- shared topology 수준 `shared_global_block`에서는 TPS 중앙값이 약 `0.98M`, p99 중앙값이 약 `299.54us`이었습니다.

> primitive: 큐 자체만 측정
> shared topology: 전체 토폴로지 기준 측정
> ceiling_drop: 꽉차면 요청 버림
> hared_global_block: 큐를 공유하고, 글로벌 락을 잡고, 꽉차면 기다림

---

## Split Lock Queue

global lock의 문제는 큐 전체를 하나의 덩어리로 잠근다는 점입니다.

그렇다면 자연스러운 개선은?

> push와 pop을 나눠볼까?

- push는 tail 쪽만 건드린다.
- pop은 head 쪽만 건드린다.
- 따라서 enqueue와 dequeue가 항상 같은 락에서 부딪히지 않아도 된다.


```cpp
bool push(Event ev, BackpressurePolicy policy,
          const std::atomic<bool>& stop_flag) override {
  if (slots_ && !slots_->try_acquire()) {
    if (policy == BackpressurePolicy::Drop) {
      return false;
    }

    while (!slots_->try_acquire()) {
      if (closed_.load(std::memory_order_acquire) ||
          stop_flag.load(std::memory_order_relaxed)) {
        return false;
      }
      waiter.pause();
    }
  }

  Node* node = new Node(std::move(ev));

  {
    std::lock_guard<std::mutex> lock(tail_mu_);
    tail_->next = node;
    tail_ = node;
  }

  items_.release();
  return true;
}

bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
  while (!items_.try_acquire()) {
    if (closed_.load(std::memory_order_acquire) ||
        stop_flag.load(std::memory_order_relaxed)) {
      return false;
    }
    waiter.pause();
  }

  {
    std::lock_guard<std::mutex> lock(head_mu_);
    Node* old_head = head_;
    Node* new_head = old_head->next;

    out = std::move(*(new_head->value));
    head_ = new_head;

    delete old_head;
  }

  if (slots_) slots_->release();
  return true;
}

private:
  std::mutex head_mu_;
  std::mutex tail_mu_;
  std::counting_semaphore<> items_;
  std::unique_ptr<std::counting_semaphore<>> slots_;
```

핵심은.. 

```cpp
std::lock_guard<std::mutex> lock(tail_mu_);
std::lock_guard<std::mutex> lock(head_mu_);
```

push는 `tail_mu_`를 잡고, pop은 `head_mu_`를 잡습니다.

즉 global lock처럼 모든 동작이 하나의 `mu_`에 몰리지 않습니다.

### Semaphore는 왜 필요할까요?

코드에 `items_`와 `slots_`가 나옵니다.

이 둘은 `mutex`와 역할이 다릅니다.

`mutex`는 큐 내부 자료구조를 실제로 변경하는 구간을 보호합니다.  
즉 여러 thread가 동시에 `head`, `tail`, `next` 포인터를 수정하지 못하게 막는 장치입니다.

반면 `semaphore`는 “지금 접근해도 되는가”를 수량으로 관리합니다.  
이 코드에서는 큐 안에 꺼낼 item이 몇 개 있는지, 그리고 새로 넣을 수 있는 빈 slot이 몇 개 남았는지를 나타냅니다.

- `items_`: consumer가 꺼낼 수 있는 item 수
- `slots_`: producer가 넣을 수 있는 빈자리 수

예를 들어 `consumer`는 먼저 `items_`를 확인합니다.  
`items_`가 0이면 큐에 꺼낼 item이 없다는 뜻이므로 기다리거나 실패해야 합니다.  

반대로 `producer`는 먼저 `slots_`를 확인합니다.  
`slots_`가 0이면 큐가 꽉 찼다는 뜻이므로 기다리거나 drop해야 합니다.

그 다음에야 실제 큐 구조를 변경합니다.

- push는 `tail_mu_`를 잡고 tail 쪽 링크를 수정합니다.
- pop은 `head_mu_`를 잡고 head 쪽 링크를 수정합니다.

즉 split lock 구조에서는 역할이 분리됩니다.

- 자료구조 접근 보호: `head_mu_`, `tail_mu_`
- queue 상태 관리: `items_`, `slots_`

이 구조는 비교적 복잡합니다.  
linked-list node 할당/해제가 필요하고, semaphore도 관리해야 하며, depth 같은 부가 상태도 맞춰야 합니다.

`global_lock`은 push와 pop이 모두 같은 mutex를 잡기 때문에 producer와 consumer가 한 지점에서 계속 부딪힙니다.  
반면 `split_lock`은 push 경로와 pop 경로를 나누고, 큐가 비었는지/가득 찼는지는 semaphore로 관리합니다.

결국 핵심은 이것입니다.

`mutex`는 “누가 큐 내부 구조를 수정할 수 있는가”를 막고,  
`semaphore`는 “지금 꺼낼 item이나 넣을 slot이 존재하는가”를 관리합니다.

### 실험에서 보인 의미

split lock은 global lock보다 확실히 나은 결과를 보였습니다.

- primitive 수준 `ceiling_drop` 기준으로 TPS 중앙값은 약 `2.81M`, p99 중앙값은 약 `82.75us`였다.
- shared topology 수준 `shared_split_zero_block`에서도 TPS 중앙값은 약 `1.80M`, p99 중앙값은 약 `177.62us`로 관찰되었다.

같은 shared queue 전제를 유지한다면, global lock보다 split lock이 훨씬 현실적인 기본형으로 보입니다.

---

## Ring Buffer

고정된 크기의 배열을 원형으로 돌려 쓰는 queue입니다.

![](https://velog.velcdn.com/images/genius00hwan/post/068d7b69-b61e-40f9-bfb5-83c938f881b1/image.png)


일반적인 queue는 뒤에 데이터를 넣고 앞에서 데이터를 꺼냅니다.  
배열로 queue를 만들면 한쪽 방향으로 계속 밀려나기 때문에, 공간을 재사용하려면 데이터를 앞으로 당기거나 새로운 공간을 잡아야 할 것처럼 보입니다.

Ring Buffer는..

고정된 배열을 만들고, 마지막 칸 다음은 다시 첫 번째 칸으로 이어진다고 생각합니다.  
그래서 index가 배열 끝에 도달하면 0으로 돌아갑니다.

```cpp
std::size_t nextIndex(std::size_t index) const {
  return (index + 1) % capacity_;
}
```


### 왜 Ring Buffer를 쓰는가

Ring Buffer의 장점은 명확합니다.

- 메모리를 미리 잡아두고 재사용합니다.
linked queue처럼 매번 node를 new로 만들고 delete할 필요가 없습니다.

- 배열 기반이라 cache locality가 좋습니다.
연속된 메모리 공간을 사용하기 때문에 CPU cache 관점에서 유리할 수 있습니다.

- bounded queue를 만들기 쉽습니다.
capacity가 고정되어 있으므로 큐가 어디까지 커질 수 있는지 예측할 수 있습니다.

하지만 단점도 있습니다.

capacity가 고정되어 있으므로 큐가 가득 찰 수 있습니다.
따라서 큐가 꽉 찼을 때 producer를 기다리게 할지, 아니면 새 이벤트를 버릴지 정책이 필요합니다.


## SPSC Ring Queue

_Single Producer Single Consumer_

말 그대로 producer 하나, consumer 하나만 허용합니다.

이 구조는 일반적인 shared queue와 비교할 때 매우 강해 보입니다. 실제로 primitive 수준에서는 높은 처리량과 낮은 tail을 보였습니다.

- `ceiling_drop` 기준 TPS 중앙값은 약 `4.28M`
- p99 중앙값은 약 `59.12us`

SPSC는 명확한 조건이 있습니다.

- producer는 하나여야 한다.
- consumer도 하나여야 한다.
- 이 조건이 깨지면 버려야 됨.

그니까 개발자가 정책으로 효율화한 느낌입니다. 범용적으로는 쓰기 어렶습니다.


```cpp
bool push(Event ev, BackpressurePolicy policy,
          const std::atomic<bool>& stop_flag) override {
  while (true) {
    const auto tail = tail_.value.load(std::memory_order_relaxed);
    const auto next = nextIndex(tail);
    const auto head = head_.value.load(std::memory_order_acquire);

    if (next != head) {
      buffer_[tail] = std::move(ev);
      tail_.value.store(next, std::memory_order_release);
      return true;
    }

    if (policy == BackpressurePolicy::Drop) {
      return false;
    }

    waiter.pause();
  }
}

bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
  while (true) {
    const auto head = head_.value.load(std::memory_order_relaxed);
    const auto tail = tail_.value.load(std::memory_order_acquire);

    if (head != tail) {
      out = std::move(buffer_[head]);
      head_.value.store(nextIndex(head), std::memory_order_release);
      return true;
    }

    if (closed_.load(std::memory_order_acquire) ||
        stop_flag.load(std::memory_order_relaxed)) {
      return false;
    }

    waiter.pause();
  }
}

private:
  std::vector<Event> buffer_;
  PaddedIndex head_{};
  PaddedIndex tail_{};
```

여기에는 `std::mutex`가 없죠?

producer는 주로 `tail`을 움직이고, consumer는 주로 `head`를 움직입니다. 서로 만지는 부분이 다르기 때문에 mutex 없이도 성립합니다.

물론 `head`와 `tail`을 서로 읽어야 하므로 memory ordering은 필요하다.

- producer는 `head`를 보고 큐가 꽉 찼는지 확인한다.
- consumer는 `tail`을 보고 큐가 비었는지 확인한다.
- store에는 release, 상대편이 보는 load에는 acquire가 들어갑니다.

그래서 빠릅니다.

### SPSC를 해석할 때 주의할 점

SPSC를 `global_lock`, `split_lock`, `mpmc_ring` 같은 shared queue와 동등하게 비교하는 걸 주의해야 합니다.

- `global_lock`, `split_lock`, `mpmc_ring`: 여러 producer와 여러 consumer가 공유하는 큐를 어떻게 구현할 것인가?
- `spsc`: producer 하나, consumer 하나를 전제할 수 있는가?

질문이 다르다.

그래서 SPSC는 보통 worker-local topology (전반적인 큰 그림)안에서 봐야 ㅎ합니다.

---

## Shared MPMC Ring

_Multi Producer Multi Consumer_


여러 producer와 여러 consumer가 동시에 쓰는 큐입니다.

`shared_mpmc_ring`은 고정 크기의 원형 버퍼를 shared queue로 사용합니다. 
미리 잡아 둔 배열 공간을 빙글빙글 돌려 씁니다.

capacity가 256이면 동시에 256개까지만 놓을 수 있습니다. 그 이상은 기다리거나 버려야 합니다


```cpp
bool push(Event ev, BackpressurePolicy policy,
          const std::atomic<bool>& stop_flag) override {
  if (!slots_available_.try_acquire()) {
    if (policy == BackpressurePolicy::Drop) {
      return false;
    }

    while (!slots_available_.try_acquire()) {
      if (closed_.load(std::memory_order_acquire) ||
          stop_flag.load(std::memory_order_relaxed)) {
        return false;
      }
      waiter.pause();
    }
  }

  {
    std::lock_guard<std::mutex> lock(tail_mu_);
    buffer_[tail_] = std::move(ev);
    tail_ = nextIndex(tail_);
  }

  items_.release();
  return true;
}

bool pop(Event& out, const std::atomic<bool>& stop_flag) override {
  while (!items_.try_acquire()) {
    if (closed_.load(std::memory_order_acquire) ||
        stop_flag.load(std::memory_order_relaxed)) {
      return false;
    }
    waiter.pause();
  }

  {
    std::lock_guard<std::mutex> lock(head_mu_);
    out = std::move(buffer_[head_]);
    head_ = nextIndex(head_);
  }

  slots_available_.release();
  return true;
}
```

MPMC ring도 ring buffer를 쓴다고 해서 무조건 lock-free가 되는 것은 아닙니다.

이 구현에서는 여러 producer와 여러 consumer가 같은 ring을 공유하므로, head/tail 보호와 semaphore가 필요합니다.

### 장점과 한계

장점은 고정 메모리입니다.

linked queue처럼 node allocation이 계속 발생하지 않습니다. capacity도 명확합니다. ring이 잘 맞는 workload에서는 높은 throughput을 보일 수 있습니다.

하지만 여전히 shared queue입니다.

producer와 consumer는 같은 ring 상태를 두고 경쟁하는 데, capacity가 작거나 overload가 강하면 `full_hits`가 늘고, block 모드에서는 기다림이 늘고, drop 모드에서는 손실이 늘 수 있습니다.

실험 결과에서도 shared family 내부에서는 이런 경향이 보였습니다.

- block 조건에서는 MPMC ring이 높은 throughput 을 보입니다.
- overload drop 조건에서는 split lock이 MPMC ring보다 더 나은 TPS와 tail을 보이며 안정적인 shared의 기본형처럼 보였습니다.

---

## 이제 큐 말고 전체 구조를 봅시다

지금까지는 순사하게 queue primitive 중심으로 봤습니다.

하지만 실제 시스템에서는 큐 하나만 던져 놓고 끝나지 않습니다. 큐가 어느 위치에 놓이는지, 작업이 어떤 경로를 지나 worker에게 도달하는지가 중요합니다.

그래서 비교를 두 set로 나눴습니다.
~~family 라는 이름은 codex가 지어줬습니다~~

| Family | 비교 대상 | 질문 |
| --- | --- | --- |
| Shared Queue Family | `shared_split_lock`, `shared_mpmc_ring` | 하나의 shared queue를 유지한다면 무엇이 나은가? |
| Local Queue Family | `dispatch_local_spsc`, `direct_local_spsc` | worker-local SPSC를 허용한다면 어떤 배선이 나은가? |

이 둘의 차이는

shared queue family는 중앙 큐 하나를 어떻게 더 잘 만들 것인지를 봐야 합니다.

local queue family는 작업 흐름을 worker별로 어떻게 나눌 것인지를 봐야 합니다.

---

## Dispatch Local SPSC

`dispatch_local_spsc`는 worker마다 SPSC queue를 두되, 앞단에 dispatcher를 둔다.

구조는 대략

```text
Ingress Threads
      │
      ▼
Dispatch Queue
      │
      ▼
Dispatcher
      │
      ├──> Worker 0 SPSC Queue ──> Worker 0
      ├──> Worker 1 SPSC Queue ──> Worker 1
      └──> Worker 2 SPSC Queue ──> Worker 2
```


모든 요청이 Dispatcher로 들어옵니다. dispatcher는 각 물건을 어느 worker line으로 보낼지 결정합니다.

```cpp
// ingress thread
Event ev{};
ev.target_worker = ev.route_key % config.worker_threads;

dispatch_queue->push(
    std::move(ev),
    config.policy,
    state.ingress_stop
);

// dispatcher thread
while (dispatch_queue->pop(ev, pop_stop)) {
  auto target = ev.target_worker %
                static_cast<std::size_t>(config.worker_threads);

  auto& worker_queue = worker_queues[target];

  const bool ok = worker_queue->push(
      std::move(ev),
      config.policy,
      pop_stop
  );

  if (!ok && config.policy == BackpressurePolicy::Drop) {
    state.dropped_dispatch.fetch_add(1, std::memory_order_relaxed);
  }
}
```

여기서 이벤트는 worker에게 바로 가지 않습니다.

1. ingress가 dispatch queue에 넣는다.
2. dispatcher가 dispatch queue에서 꺼낸다.
3. dispatcher가 worker-local SPSC queue에 다시 넣는다.
4. worker가 자기 queue에서 꺼내 처리한다.

즉 queue hop이 하나 늘어납니다.

### 장점

중앙에서 분배를 통제할 수 있습니다.

작업을 worker에게 로드밸런싱하거나, 나중에 복잡한 routing policy를 넣기 좋습니다. 실제 결과에서도 `dispatch_local_spsc`는 worker imbalance를 낮게 유지하는 장점이 있었습니다.

분배 품질이 중요한 시스템에서는 dispatcher가 꽤 유용합니다.

### 단점

경로가 길어집니다.

큐를 한 번 더 통과한다는 것은 기다릴 기회가 한 번 더 생긴다는 뜻입니다. 특히 block 모드에서는 이 추가 hop이 p99에 부담이 될 수 있습니다.

현재 결과에서도 local family 내부에서 `dispatch_local_spsc`는 중앙 분배 덕분에 imbalance는 낮았지만, `direct_local_spsc`보다 p99가 높게 관찰되었습니다.

즉 dispatcher는 균형에는 좋지만 tail에는 비용이 될 수 있습니다.

---

## Direct Local SPSC

`direct_local_spsc`는 dispatcher를 없앱니다.

Ingress가 바로 worker-local SPSC queue에 꽂아 넣습니다.

```text
Ingress 0 ──> Worker 0 SPSC Queue ──> Worker 0
Ingress 1 ──> Worker 1 SPSC Queue ──> Worker 1
Ingress 2 ──> Worker 2 SPSC Queue ──> Worker 2
```


```cpp
// direct_local_spsc: fixed ingress-to-worker affinity
const auto target_worker =
    static_cast<std::size_t>(ingress_index % config.worker_threads);

Event ev{};
ev.id = (static_cast<std::uint64_t>(ingress_index) << 48) | seq++;
ev.route_key = ev.id;
ev.target_worker = target_worker;
ev.created_at = SteadyClock::now();

auto& worker_queue = worker_queues[target_worker];

const bool ok = worker_queue->push(
    std::move(ev),
    config.policy,
    state.ingress_stop
);

if (!ok && config.policy == BackpressurePolicy::Drop) {
  state.dropped_ingress.fetch_add(1, std::memory_order_relaxed);
}
```

여기서는 dispatcher가 없다.

Ingress가 target worker를 정하고 곧바로 worker queue에 넣습니다.

### 제약

이 구조는 아무 조건에서나 쓸 수 없습니다.

구현에는 topology별 제약이 명시되어 있습니다.

```cpp
if (config.topology == PipelineTopology::SharedQueue) {
  if (config.shared_queue_kind == QueueKind::SpscRing) {
    throw std::runtime_error(
        "shared_queue topology에는 spsc를 직접 사용할 수 없습니다."
    );
  }
  return;
}

if (config.dispatch_queue_kind == QueueKind::SpscRing) {
  throw std::runtime_error(
      "local_spsc topology의 dispatch queue는 shared queue여야 합니다."
  );
}

if (config.topology == PipelineTopology::DirectLocalSpsc &&
    config.ingress_threads > config.worker_threads) {
  throw std::runtime_error(
      "direct_local_spsc는 ingress_threads <= worker_threads 조건이 필요합니다."
  );
}
```

여기서 중요한 것은 `SpscRingQueue`를 shared queue topology에 직접 넣을 수 없다는 점입니다.

SPSC는 1P/1C 계약이 필요합니다. 여러 producer와 여러 consumer가 달라붙는 shared queue 자리에 그대로 꽂으면 안 됩니다.

또한 direct local 구조는 현재 구현상 `ingress_threads <= worker_threads` 조건이 필요합니다.

### 장,단점

장점: 짧은 경로

dispatcher hop이 없으므로 block 모드에서 p99를 줄이기 쉽습니다. 실제 결과에서도 `direct_local_spsc`는 `dispatch_local_spsc`보다 낮은 tail을 보였고, 현재 조건에서는 shared split lock보다도 더 좋은 처리량과 tail을 보였습니다.

단점: 정책 짜기

routing 품질에 민감합니다. 특정 ingress나 route가 특정 worker로만 몰리면 hot shard가 생길 수 있습니다. dispatcher가 없으므로 중앙에서 균형을 잡아 주는 게 어렵습니다.

---

## 지표

벤치마크에서 제일 위험한 문장은 이것입니다.

> TPS가 가장 높으니 최고다.

"Technically" 어떤 workload인지, 어떤 backpressure policy인지에 따라 먼저 봐야 할 지표가 달라집니다.

### `tps_completed`

`tps_completed`는 초당 실제로 완료한 작업 수입니다.

처리량을 보여 주는 핵심 지표

특히 `overload + drop` 조건에서는 매우 중요하다. 이 조건에서는 애초에 모든 작업을 보존할 수 없다. 그러면 중요한 질문은 “제한된 자원으로 얼마나 많은 일을 완료했는가”가 된다.

하지만 TPS만 보면 안 된다.

drop이 너무 많아서 남은 요청만 빠르게 처리했을 수도 있다. p99가 심하게 튈 수도 있다. 특정 worker만 과열되어 전체 TPS는 높아도 실제 운영 안정성은 낮을 수 있다.

### 평균 latency

평균 latency는 전체 요청의 평균 지연입니다.

평균은 tail을 숨깁니다. 대부분 빠르고 일부가 매우 느린 경우에도 평균은 멀쩡해 보일 수 있습니다.

그래서 평균 latency는 보조 지표로 봤습니다.

### p95와 p99

p95는 5%의 느린 요청이 어느 정도인지 보여 줍니다.

p99는 1%의 꼬리를 보여 줍니다.

block 모드에서는 p99가 특히 중요합니다. 
block은 작업을 버리지 않고 기다리게 하는 정책이기 때문에, 시스템이 밀리면 tail부터 길어집니다.

- TPS가 충분히 나온다.
- p99가 과도하게 튀지 않는다.
- queue depth가 capacity 근처에서 계속 비명을 지르지 않는다.
- worker imbalance가 심하지 않다.

### Drop 위치

drop 모드에서는 단순히 drop 수만 보면 안 돼여. 어디서 drop이 발생했는지가 중요합니다.

- `dropped_ingress`: 입력 단계에서 버려짐
- `dropped_dispatch`: dispatcher 이후 worker queue로 보내는 과정에서 버려짐

예를 들어 `dispatch_local_spsc`에서 `dropped_dispatch`가 많다면, ingress는 받았지만 dispatcher가 worker-local queue로 내려보내는 구간에서 막힌다는 뜻일 수 있습ㄴ다.

즉 drop 위치는 병목 위치를 알려줍니다.

### Queue Depth와 `max_observed_depth`

queue depth는 큐에 얼마나 쌓였는지를 보여 줍니다.

`max_observed_depth`: 측정 중 관찰된 최대 깊이

이 값이 높으면 backlog가 쌓였다는 뜻입니다.

- consumer가 못 따라간다.
- 특정 worker queue만 밀린다.
- dispatcher가 밀린다.
- capacity 압박이 크다.

### `full_hits`와 `empty_hits`

`full_hits`: push하려고 했는데 큐가 가득 찬 횟수
높으면 capacity 압박이나 overload를 의심하자

`empty_hits`: pop하려고 했는데 큐가 비어 있던 횟수

높으면 consumer가 자주 놀고 있다는 뜻. 시스템에 여유가 있어서 큐가 자주 비는 것일 수도 있으니 다른 지표와 같이 봐야 함.

### `worker_imbalance_pct`

local topology에서는 worker imbalance를 반드시 봐야 합니다.

local queue는 경합을 줄이는 대신 작업 분배 문제를 새로 만드는데, 전체 TPS가 좋아 보여도 특정 worker만 과열되면 tail이 나빠질 수 있습니다.

`worker_imbalance_pct`가 낮으면 worker별 완료량이 비교적 균등하다는 뜻입니다.

`worker_imbalance_pct`가 높으면 route skew, 잘못된 mapping, hot shard 가능성을 의심해야 합니다.

---

## 결과를 봅시다

큐 벤치마크 결과는 조건에 따라 읽는 순서가 달라집니다.  
같은 TPS, 같은 p99라도 어떤 부하 조건에서 나온 값인지에 따라 의미가 완전히 달라질 수 있습니다.

특히 이번 실험에서는 크게 두 가지 상황을 나누어 봐야 합니다.

첫 번째는 시스템이 거의 포화 상태에 가까운 상황에서 작업을 버리지 않고 기다리는 `near_sat + block` 조건입니다.  
두 번째는 시스템이 감당하기 어려운 부하를 받는 상황에서 일부 작업을 버리는 `overload + drop` 조건입니다.

둘은 질문 자체가 다릅니다.

`near_sat + block`에서는 “작업을 버리지 않고 기다릴 때 tail latency가 얼마나 안정적인가?”를 봐야 합니다.  
반면 `overload + drop`에서는 “과부하 상황에서 얼마나 많은 작업을 완료했고, 어디에서 drop이 발생했는가?”를 봐야 합니다.

이 차이를 구분하지 않으면 벤치마크 결과를 쉽게 잘못 읽게 됩니다.

먼저 표로 한 번 잡고 가면 아래 문단이 더 쉽게 읽힙니다.

| 조건 | 먼저 볼 지표 | shared family에서 읽을 포인트 | local family에서 읽을 포인트 | 현재 관찰된 방향 |
| --- | --- | --- | --- | --- |
| `near_sat + block` | `p99 -> tps_completed` | shared queue를 유지한다면 tail이 덜 튀는지, queue hop 없이 안정적으로 버티는지 본다 | dispatcher hop이 p99를 얼마나 키우는지, 대신 imbalance를 얼마나 줄여 주는지 본다 | `shared_split_lock`, `direct_local_spsc`가 각각 자기 family 안에서 더 설득력 있게 보였다 |
| `overload + drop` | `tps_completed -> drop 위치 -> p99` | bounded ring의 capacity 압박이 얼마나 빨리 드러나는지, drop과 full hit가 어디서 커지는지 본다 | ingress에서 버려지는지, dispatch 단계에서 막히는지 보고 병목 위치를 추정한다 | `shared_split_lock`, `direct_local_spsc`가 각각 더 안정적인 후보처럼 보였다 |

---

### `near_sat + block`

`near_sat + block`은 시스템이 거의 포화에 가까운 상태에서 작업을 버리지 않고 기다리는 조건입니다.

이 조건에서는 먼저 `p99`를 봐야 합니다.

`block` 정책은 큐가 꽉 찼을 때 producer를 멈춰 세우는 방식입니다.  
즉 작업 손실은 줄일 수 있지만, 그 대신 기다림이 생깁니다.  
이 기다림이 길어지면 평균보다 먼저 p99가 튀기 시작합니다.

따라서 읽는 순서는 대략 다음과 같습니다.

`p99 → tps_completed → max_observed_depth → full_hits → wait count → worker_imbalance_pct`

여기서 TPS만 보고 승자를 정하면 안 됩니다.

처리량이 높아도 p99가 크게 튄다면 실제 서비스에서는 위험한 구조일 수 있습니다.  
사용자는 평균적으로 빠른 시스템보다, 가끔 심하게 느려지는 시스템을 더 불안정하게 느끼기 때문입니다.

이번 결과에서는 `direct_local_spsc` 구조가 `dispatch_local_spsc`보다 더 낮은 p99를 보였습니다.  
이는 dispatcher를 한 번 거치는 추가 hop이 block 조건의 tail latency에 부담이 될 수 있음을 보여 줍니다.

`dispatch_local_spsc`는 중앙 dispatcher를 통해 worker-local queue로 작업을 분배합니다.  
이 방식은 worker 간 균형을 잡는 데 유리하지만, 작업이 worker에게 도달하기 전에 한 번 더 큐를 통과해야 합니다.

반면 `direct_local_spsc`는 ingress가 worker-local queue에 직접 작업을 넣습니다.  
중간 dispatcher hop이 없기 때문에 경로가 짧고, 그만큼 tail latency 측면에서 유리하게 나타날 수 있습니다.

다만 이것을 단순히 “direct local이 항상 더 좋다”로 해석하면 안 됩니다.  
현재 조건에서는 route가 비교적 안정적이고, direct mapping이 잘 맞는 상황이었기 때문에 좋은 결과가 나온 것입니다.

즉 이 조건에서 배울 점은 다음과 같습니다.

> block 조건에서는 작업을 버리지 않는 대신 기다림이 생기므로, TPS보다 p99를 먼저 봐야 합니다.  
> 그리고 p99가 커지는 원인이 락 경합인지, queue hop인지, worker imbalance인지 함께 추적해야 합니다.

---

### `overload + drop`

`overload + drop`은 시스템이 감당하기 어려운 부하를 받는 조건입니다.

이 조건에서는 먼저 `tps_completed`를 봐야 합니다.

왜냐하면 이 상황에서는 모든 작업을 처리할 수 없다는 전제가 깔려 있기 때문입니다.  
따라서 핵심 질문은 “얼마나 많은 작업을 실제로 완료했는가?”입니다.

읽는 순서는 대략 다음과 같습니다.

`tps_completed → dropped_ingress / dropped_dispatch → full_hits → p99 → max_observed_depth`

여기서 중요한 점은 p99를 너무 빨리 믿으면 안 된다는 것입니다.

drop 정책에서는 큐가 꽉 찼을 때 작업을 버릴 수 있습니다.  
앞단에서 많은 작업을 빠르게 버리면, 실제로 처리된 작업들만 놓고 봤을 때 p99가 낮아 보일 수 있습니다.

즉 p99가 낮다고 해서 반드시 좋은 구조라는 뜻은 아닙니다.  
“처리된 작업은 빨랐지만, 너무 많은 작업이 버려졌을 가능성”도 함께 봐야 합니다.

그래서 drop 조건에서는 drop 위치가 중요합니다.

- `dropped_ingress`: 입력 단계에서 작업이 버려진 경우
- `dropped_dispatch`: dispatcher 이후 worker-local queue로 보내는 과정에서 버려진 경우

drop 위치는 병목이 어디에 있는지를 알려 줍니다.

예를 들어 `dropped_dispatch`가 많다면 ingress는 작업을 받아냈지만, dispatcher 이후 worker-local queue로 내려보내는 과정에서 막혔다는 뜻일 수 있습니다.  
반면 `dropped_ingress`가 많다면 입력 단계에서 이미 시스템이 수용 한계를 넘었다고 볼 수 있습니다.

현재 결과에서는 shared family 안에서 `split_lock`이 `shared_mpmc_ring`보다 overload drop 조건에서 더 설득력 있는 결과를 보였습니다.  
이는 bounded ring 구조가 항상 overload에 강한 것은 아니라는 점을 보여 줍니다.

ring buffer는 고정된 공간을 재사용한다는 장점이 있지만, capacity가 고정되어 있기 때문에 과부하에서는 full hit가 빠르게 증가할 수 있습니다.  
즉 공간이 명확히 제한된다는 장점이, overload 조건에서는 압박으로 돌아올 수 있습니다.

local family에서는 `direct_local_spsc`가 `dispatch_local_spsc`보다 처리량과 tail 모두에서 더 좋은 모습을 보였습니다.  
dispatcher hop을 제거한 것이 overload 조건에서도 이점으로 작용한 것으로 볼 수 있습니다.

다만 이 역시 현재 조건에서의 해석입니다.  
route skew가 심해지거나 특정 worker에 작업이 몰리는 조건에서는 direct local의 결과가 달라질 수 있습니다.

이 조건에서 배울 점은 다음과 같습니다.

> overload + drop 조건에서는 TPS와 drop 위치를 먼저 봐야 합니다.  
> p99가 낮아도 많은 작업을 앞에서 버린 결과일 수 있으므로, drop 수와 drop 위치를 함께 해석해야 합니다.

---

### Uniform Route

Uniform route는 작업이 worker들에게 비교적 균등하게 나뉘는 조건입니다.

이 조건에서는 local queue 구조가 장점을 보이기 쉽습니다.  
작업이 worker-local queue에 고르게 분산되면, 각 worker는 자기 queue를 중심으로 독립적으로 처리할 수 있습니다.

이렇게 되면 shared queue에서 발생하는 중앙 경합을 줄일 수 있습니다.

shared queue에서는 여러 producer와 consumer가 하나의 큐를 함께 사용합니다.  
따라서 아무리 큐 구현을 개선해도 중앙에 공유 지점이 남습니다.

반면 local queue는 작업을 여러 worker-local queue로 나누기 때문에, 경합 지점을 분산할 수 있습니다.

하지만 uniform route라고 해서 worker imbalance를 안 봐도 되는 것은 아닙니다.

균등하게 분산된다고 가정했는데 실제 결과에서 특정 worker만 많은 작업을 처리했다면, routing 방식이나 affinity 설정에 문제가 있을 수 있습니다.

따라서 uniform route 조건에서는 다음을 함께 봐야 합니다.

`tps_completed → p99 → worker_imbalance_pct → worker별 max_observed_depth`

이 조건에서 배울 점은 다음과 같습니다.

> 작업이 균등하게 분산될 수 있다면 worker-local queue는 shared queue보다 경합을 줄이는 데 유리합니다.  
> 하지만 실제로 균등하게 분산되었는지는 worker imbalance와 worker별 queue depth로 확인해야 합니다.

---

### Skewed Route

현실 시스템은 항상 균등하지 않습니다.

특정 사용자, 특정 상품, 특정 계정, 특정 partition으로 요청이 몰릴 수 있습니다.  
이런 상황을 route skew라고 볼 수 있습니다.

route skew가 심해지면 local queue 구조의 약점이 드러날 수 있습니다.

local queue는 worker마다 queue를 나누기 때문에, 작업이 고르게 분산되면 강합니다.  
하지만 특정 worker queue에만 작업이 몰리면 그 queue만 계속 쌓이고, 다른 worker는 상대적으로 놀 수 있습니다.

이 경우 전체 worker 수가 많아도 실제로는 일부 worker만 병목이 됩니다.

따라서 skew 조건에서는 다음 지표를 중요하게 봐야 합니다.

- `worker_imbalance_pct`
- 특정 worker queue의 `max_observed_depth`
- `p99`
- `drop location`
- route distribution

특히 direct local 구조는 fixed affinity에 가깝기 때문에 route skew에 민감할 수 있습니다.  
중앙 dispatcher가 없다는 것은 경로가 짧다는 장점이지만, 동시에 불균형을 중간에서 보정하기 어렵다는 뜻이기도 합니다.

현재 결과는 skew 조건을 충분히 포함하지 않습니다.  
따라서 direct local이 현재 조건에서 좋아 보인다고 해서, 심한 skew에서도 항상 좋다고 일반화하면 안 됩니다.

이 조건에서 배울 점은 다음과 같습니다.

> local queue는 경합을 줄이는 대신 분배 품질에 의존합니다.  
> route skew가 심한 시스템이라면 direct local 구조를 선택하기 전에 hot worker, hot shard, worker imbalance를 반드시 검증해야 합니다.

---

## 구조별 선택 기준

이제 각 구조를 어떤 상황에서 선택할 수 있는지 정리해 보겠습니다.

중요한 것은 하나의 만능 구조를 고르는 것이 아닙니다.  
각 구조는 서로 다른 비용을 지불하고, 서로 다른 문제를 해결합니다.

---

### `global_lock`

`global_lock`은 가장 단순한 구조입니다.

큐 전체를 하나의 mutex로 보호합니다.  
push도 같은 락을 잡고, pop도 같은 락을 잡습니다.

이 구조의 장점은 명확합니다.

구현이 쉽고, correctness를 설명하기 쉽습니다.  
동시에 하나의 thread만 큐 내부를 수정할 수 있으므로, 동시성 버그를 피하기도 상대적으로 쉽습니다.

따라서 적합한 상황은 다음과 같습니다.

- baseline이 필요할 때
- 병렬성이 작을 때
- correctness를 가장 쉽게 설명하고 싶을 때
- 구현 복잡도를 낮춰야 할 때
- 성능보다 단순성이 더 중요할 때

하지만 병렬성이 커지면 한계도 명확합니다.

여러 producer와 consumer가 모두 같은 mutex를 잡으려고 하기 때문에, 경합이 한 지점에 집중됩니다.  
fan-in이 커질수록 이 단일 락은 병목이 되기 쉽습니다.

피해야 할 상황은 다음과 같습니다.

- producer와 consumer가 많이 붙을 때
- shared queue fan-in이 클 때
- p99 tail이 중요한 서비스일 때
- 큐 접근 빈도가 높은 hot path일 때

한 줄로 정리하면 다음과 같습니다.

> global lock은 출발점으로는 좋지만, 고성능 shared queue의 최종 답으로 보기에는 한계가 있습니다.

---

### `split_lock`

`split_lock`은 shared queue를 유지하면서 경합을 줄이고 싶을 때 현실적인 구조입니다.

핵심 아이디어는 push 경로와 pop 경로의 락을 분리하는 것입니다.  
push는 tail 쪽 락을 잡고, pop은 head 쪽 락을 잡습니다.

즉 global lock처럼 큐 전체를 하나의 mutex로 막지 않습니다.

이 구조가 적합한 상황은 다음과 같습니다.

- 기존 topology를 크게 바꾸기 어렵다.
- shared queue 하나를 유지해야 한다.
- global lock 병목이 보인다.
- producer와 consumer가 동시에 많이 붙는다.
- local queue로 구조를 바꾸기에는 운영 복잡도가 부담된다.

물론 단점도 있습니다.

`split_lock`은 global lock보다 구현이 복잡합니다.  
semaphore, node allocation, head/tail lock, depth 관리가 들어갑니다.  
또한 linked-list 기반이라 node 할당과 해제 비용도 고려해야 합니다.

하지만 shared queue 구조를 유지하면서 성능을 개선하려면 이 복잡도는 꽤 설득력 있는 비용입니다.

현재 결과에서도 shared queue 기본형으로는 `split_lock`이 가장 안정적인 후보처럼 보입니다.  
특히 global lock보다 경합을 줄이면서도, local topology처럼 시스템 배선 전체를 바꾸지 않아도 된다는 점이 장점입니다.

한 줄로 정리하면 다음과 같습니다.

> split lock은 shared queue를 유지해야 하는 상황에서 global lock의 병목을 줄이는 현실적인 개선안입니다.

---

### shared_mpmc_ring`

`shared_mpmc_ring`은 여러 producer와 여러 consumer가 함께 사용하는 ring buffer 구조입니다.

ring buffer는 고정된 크기의 배열을 원형으로 재사용합니다.  
linked queue처럼 매번 node를 할당하지 않고, 정해진 buffer 안에서 head와 tail을 움직이며 데이터를 넣고 뺍니다.

이 구조의 장점은 다음과 같습니다.

- capacity를 명확히 제한할 수 있다.
- 메모리 사용량을 예측하기 쉽다.
- node allocation을 줄일 수 있다.
- ring buffer의 cache locality를 기대할 수 있다.
- shared queue를 유지하면서 bounded queue를 만들 수 있다.

따라서 적합한 상황은 다음과 같습니다.

- capacity를 명확히 제한하고 싶다.
- node allocation을 줄이고 싶다.
- shared queue를 유지해야 한다.
- bounded memory가 중요하다.
- block 조건에서 높은 throughput 잠재력을 보고 싶다.

하지만 주의할 점도 있습니다.

MPMC ring은 SPSC ring과 다릅니다.  
둘 다 ring buffer를 사용하지만, MPMC는 여러 producer와 여러 consumer가 동시에 접근합니다.

따라서 head/tail 상태를 둘러싼 동기화 비용이 남습니다.  
ring이라고 해서 SPSC처럼 가벼운 hot path를 그대로 기대하면 안 됩니다.

또한 bounded ring은 capacity 압박을 정면으로 받습니다.  
overload 조건에서는 full hits가 늘고, drop 또는 wait가 증가할 수 있습니다.

현재 결과에서도 `shared_mpmc_ring`은 특정 block 조건에서는 높은 throughput 잠재력을 보였지만, overload drop 조건에서는 `split_lock`보다 덜 설득력 있는 모습을 보였습니다.

한 줄로 정리하면 다음과 같습니다.

> MPMC ring은 깔끔한 bounded shared queue이지만, overload 조건에서는 capacity 압박과 동기화 비용을 반드시 따로 검증해야 합니다.

---

### 11.4 `dispatch_local_spsc`

`dispatch_local_spsc`는 worker-local SPSC queue를 사용하되, 중앙 dispatcher를 두는 구조입니다.

흐름은 대략 다음과 같습니다.

`ingress → dispatch queue → dispatcher → worker-local SPSC queue → worker`

이 구조의 장점은 중앙에서 분배 정책을 관리할 수 있다는 점입니다.

dispatcher가 각 worker queue로 작업을 나눠 보내기 때문에, worker 간 균형을 잡는 데 유리합니다.  
또한 나중에 routing policy가 복잡해져도 dispatcher에 정책을 모으기 쉽습니다.

적합한 상황은 다음과 같습니다.

- worker 간 균형이 중요하다.
- routing policy를 중앙에서 관리하고 싶다.
- 향후 복잡한 분배 전략을 넣을 가능성이 있다.
- direct mapping이 불안정하다.
- route skew를 중간에서 완화하고 싶다.

하지만 비용도 있습니다.

가장 큰 비용은 dispatcher hop입니다.  
작업이 worker에게 바로 가지 않고, dispatch queue와 dispatcher를 한 번 더 거칩니다.

이 추가 hop은 특히 block 조건에서 p99를 키울 수 있습니다.  
이번 결과에서도 `dispatch_local_spsc`는 worker balance 측면의 장점은 있지만, `direct_local_spsc`보다 p99가 높게 관찰되었습니다.

즉 이 구조는 “균형과 제어 가능성”을 얻는 대신 “경로 길이와 tail 비용”을 지불합니다.

한 줄로 정리하면 다음과 같습니다.

> dispatch local SPSC는 분배 제어에는 유리하지만, dispatcher hop이 tail latency의 비용으로 나타날 수 있습니다.

---

### 11.5 `direct_local_spsc`

`direct_local_spsc`는 dispatcher를 제거하고 ingress가 worker-local queue로 직접 작업을 넣는 구조입니다.

흐름은 다음처럼 짧습니다.

`ingress → worker-local SPSC queue → worker`

중간 dispatcher가 없기 때문에 경로가 짧습니다.  
그만큼 queue hop이 줄고, p99 측면에서 유리할 수 있습니다.

적합한 상황은 다음과 같습니다.

- ingress와 worker의 대응 관계를 자연스럽게 만들 수 있다.
- route skew가 크지 않다.
- 홉 수를 줄여 p99를 낮추는 것이 중요하다.
- `ingress_threads <= worker_threads` 같은 제약을 받아들일 수 있다.
- routing policy가 단순하다.
- fixed affinity가 잘 맞는 workload다.

현재 조건에서는 `direct_local_spsc`가 가장 눈에 띄는 결과를 보였습니다.  
처리량과 tail latency 모두에서 설득력 있는 모습을 보였습니다.

하지만 이것을 “항상 최고”라고 일반화하면 안 됩니다.

현재 구조는 fixed affinity 기반입니다.  
즉 ingress와 worker의 매핑이 잘 맞으면 강하지만, 특정 route가 한 worker에 몰리면 그 worker queue만 과열될 수 있습니다.

또한 `ingress_threads <= worker_threads`라는 제약도 있습니다.  
이 조건을 받아들일 수 없는 시스템에서는 그대로 적용하기 어렵습니다.

한 줄로 정리하면 다음과 같습니다.

> direct local SPSC는 경로가 짧아 현재 조건에서는 강했지만, mapping 품질과 route skew에 민감한 구조입니다.

---

## 자주 하는 오해

GPT가 알려주는 오해 바로잡기

### TPS가 높으면 짱이다?

아닙니다.

TPS는 중요하지만 혼자서는 부족합니다.

block 모드에서는 p99가 훨씬 중요할 수 있습니다.  
처리량이 높아도 p99가 크게 튀면 실제 서비스에서는 불안정하게 느껴질 수 있습니다.

drop 모드에서는 높은 TPS가 대량 drop과 함께 나온 것일 수 있습니다.  
즉 처리된 작업만 빠르게 끝났고, 많은 작업은 앞에서 버렸을 수도 있습니다.

local topology에서는 전체 TPS가 좋아 보여도 특정 worker만 과열된 상태일 수 있습니다.

좋은 해석은 이렇게 해야 합니다.

> 이 조건에서 완료 처리량은 높습니다.  
> 그런데 p99, drop 위치, worker imbalance, queue depth는 어떤가요?

TPS는 출발점이지 결론이 아닙니다.

---

### 락이 없으면 무조건 빠르다?

아닙니다.

SPSC가 강한 이유는 단순히 mutex가 없기 때문이 아닙니다.  
1 producer / 1 consumer라는 좁은 계약 덕분에 mutex가 필요 없는 구조를 만들 수 있기 때문입니다.

즉 락을 없앤 것이 아니라, 락이 덜 필요한 구조로 문제를 제한한 것입니다.

그리고 queue primitive가 빨라도 topology가 길면 전체 시스템은 느려질 수 있습니다.  
`dispatch_local_spsc`는 worker-local SPSC를 쓰지만, dispatcher hop 때문에 tail latency가 커질 수 있습니다.

좋은 질문은 이것입니다.

> 락을 줄인 대신 어떤 시스템 비용이 새로 생겼는가?

락을 없애면 끝이라는 생각은 위험합니다.  
동시성 비용은 사라지는 것이 아니라 다른 곳으로 이동하는 경우가 많습니다.

---

### drop이 있으면 무조건 나쁘다?

아닙니다.

drop은 과부하에서 시스템을 보호하기 위한 정책일 수 있습니다.

모든 요청을 무조건 받아들이면 큐가 계속 쌓이고, 결국 전체 시스템이 느려지거나 멈출 수 있습니다.  
이때 일부 요청을 빠르게 거절하는 것이 전체 시스템 안정성에는 더 나은 선택일 수 있습니다.

중요한 것은 drop이 있다는 사실 자체가 아닙니다.  
어디서, 왜 drop되었는지를 봐야 합니다.

- ingress에서 빠르게 거절했는가?
- dispatcher 뒤에서 막혔는가?
- 특정 worker queue가 꽉 차서 버렸는가?

drop 위치는 병목 위치를 알려 주는 중요한 단서입니다.

---

### shared queue보다 local queue가 무조건 낫다?

아닙니다.

local queue는 경합을 줄이는 데 유리합니다.  
하지만 routing과 imbalance 문제를 새로 만듭니다.

작업이 고르게 나뉘면 local queue는 매우 강할 수 있습니다.  
반대로 특정 route가 한 worker에 몰리면, 해당 worker queue만 병목이 되고 나머지 worker는 놀 수 있습니다.

shared queue는 중앙 병목이 있지만 단순하고 운영하기 쉽습니다.  
시스템을 크게 바꾸기 어렵다면 `shared_split_lock` 같은 개선이 더 현실적인 답일 수 있습니다.

좋은 해석은 이렇습니다.

> shared queue는 단순함을 유지하는 대신 중앙 경합을 감수합니다.  
> local queue는 경합을 줄이는 대신 분배 문제를 감수합니다.

---

## 결론

이번 실험에서 중요한 것은 조건에 따라 설계 선택의 우선순위가 달라진다는 점을 확인하는 것입니다.

1. shared queue를 유지해야 한다면 `global_lock`은 단순한 baseline에 가깝습니다.  

구현은 쉽고 correctness를 설명하기 좋지만, 병렬성이 커지면 producer와 consumer가 같은 락에서 충돌합니다.  
그 결과 tail latency가 나빠지기 쉽습니다.

2. shared queue를 유지하면서 개선하려면 `split_lock`이 현실적인 기본형입니다.  

enqueue와 dequeue 경로를 나누어 경합을 줄일 수 있고, 현재 결과에서도 global lock보다 훨씬 설득력 있는 성능을 보였습니다.  
topology 전체를 바꾸지 않아도 된다는 점에서 실무적으로도 선택하기 쉬운 편입니다.

3. `SPSC`는 더 좋은 shared queue가 아닙니다.  

1 producer / 1 consumer라는 좁은 계약을 받아들이는 대신 hot path를 매우 가볍게 만드는 구조입니다.  
따라서 SPSC는 단독 primitive로 shared queue와 나란히 비교하기보다, worker-local topology 안에서 해석해야 합니다.

4. `MPMC ring`은 bounded memory와 ring buffer의 장점이 있습니다.  

하지만 여러 producer와 여러 consumer가 같은 ring 상태를 공유하기 때문에 동기화 비용이 남습니다.  
또한 overload 조건에서는 capacity 압박을 받기 때문에 full hits, drop, tail을 반드시 함께 봐야 합니다.

5. `dispatch_local_spsc`는 worker balance를 잡기 좋지만 dispatcher hop 비용이 있습니다.  
분배를 중앙에서 제어할 수 있다는 장점이 있지만, 작업이 한 번 더 queue를 통과하기 때문에 p99가 커질 수 있습니다.  
즉 균형을 얻는 대신 경로 길이를 지불하는 구조입니다.

여섯째, `direct_local_spsc`는 현재 조건에서 처리량과 tail latency 모두 가장 설득력 있는 모습을 보였습니다.  
dispatcher hop을 제거했기 때문에 경로가 짧고, worker-local SPSC의 장점을 잘 살릴 수 있었습니다.  
하지만 fixed affinity와 `ingress_threads <= worker_threads` 제약이 있으며, skew와 fan-in이 커지면 별도 검증이 필요합니다.

결국 이번 실험의 핵심은 다음 문장으로 정리할 수 있습니다.

> 큐 벤치마크는 숫자 싸움이 아니라, 설계 계약과 병목 위치를 읽는 일입니다.

TPS만 보면 빠른 숫자에 속을 수 있습니다.  
p99만 보면 처리량과 drop을 놓칠 수 있습니다.  
SPSC만 보면 topology 비용을 놓칠 수 있습니다.  
shared queue만 고집하면 경합을 분산할 기회를 놓칠 수 있습니다.

따라서 큐 벤치마크를 읽을 때는 항상 다음 질문을 해야 합니다.

- 이 구조는 shared queue인가, worker-local queue인가?
- producer와 consumer의 동시성 계약은 무엇인가?
- 큐가 꽉 차면 기다리는가, 버리는가?
- 병목은 ingress, dispatch, worker-local queue 중 어디에서 생기는가?
- p99가 큰 이유는 락 경합인가, queue hop인가, route skew인가?
- 현재 승자는 어떤 조건에서만 승자인가?

이 질문을 할 수 있어야 벤치마크 숫자를 설계 판단으로 바꿀 수 있습니다.

숫자는 스스로 설명하지 않습니다.  
TPS, p99, drop count는 그냥 결과일 뿐입니다.  
그 숫자가 어떤 구조적 비용에서 나왔는지 해석해야 의미가 생깁니다.

이번 실험을 통해 배운 점은 명확합니다.

> 빠른 큐를 고르는 것보다 중요한 것은, 현재 시스템의 병목이 어디에 있고 어떤 비용을 감수할 수 있는지 판단하는 것입니다.

shared queue를 유지할 것인지, local queue로 나눌 것인지.  
락을 단순하게 둘 것인지, 경로를 분리할 것인지.  
작업을 기다리게 할 것인지, 일부를 버릴 것인지.  
균형을 dispatcher에게 맡길 것인지, 짧은 경로를 선택할 것인지.

큐 설계는 결국 이 선택들의 조합입니다.

