# 리터럴 String은 GC가 안되는데?

Java에서 String은 다른 메모리 관리 방식을 가진다고 알고 있었지만, 오히려 그 부분때매 헷갈렸다.

**리터럴 문자열, `new String()`으로 생성한 문자열, `intern()`된 문자열**이 GC에서 어떻게 동작하는지 확인하며,  `String Constant Pool`
`Runtime Constant Pool`과의 관계를 정리했다.

---

## 테스트 해보기

### 주요 코드

```java
String lit = "Literal";                // 리터럴
String heap = new String("Heap");      // 힙에 저장되는 일반 객체
String intern = new String("Intern");
String pooled = intern.intern();       // String Pool에 저장된 intern된 문자열

WeakReference<String> litRef = new WeakReference<>(lit, refQ);
WeakReference<String> heapRef = new WeakReference<>(heap, refQ);
WeakReference<String> internRef = new WeakReference<>(intern, refQ);
WeakReference<String> pooledRef = new WeakReference<>(pooled, refQ);

lit = null;
heap = null;
intern = null;
pooled = null;

forceGC();

System.out.println("GC 이후: 리터럴 있니~?" + (litRef.get() != null));
System.out.println("GC 이후: 스트링 객체 있니~?" + (heapRef.get() != null)); 
System.out.println("GC 이후: 인턴 된 문자열 있니~?" + (internRef.get() != null)); 
System.out.println("GC 이후: 인턴 시킨거 반환한 문자열 있니~?" + (pooledRef.get() != null));

```
### 출력 결과.

```text
GC 전: 리터럴 있니~?true
GC 전: 스트링 객체 있니~?true
GC 전: 인턴 된 문자열 있니~?true
GC 전: 인턴 시킨거 반환한 객체 있니~?true

GC 이후: 리터럴 있니~?true
GC 이후: 스트링 객체 있니~?false
GC 이후: 인턴 된 문자열 있니~?false
GC 이후: 인턴 시킨거 반환한 문자열 있니~?true

intern()한 문자열이 GC되었습니다
new String()이 GC되었습니다

```

### 왜 이런 결과가 나왔을까?

#### 처음 생각
처음엔 리터럴 문자열은 특이하게 관리되는 특이 케이스라고 생각했다.  
즉, JVM이 리터럴을 강한 참조로 붙잡고 있어서 클래스가 언로드되지 않는 한 GC되지 않는다는 식으로 단순하게 이해했다.


하지만 테스트를 진행하며, 리터럴뿐 아니라 intern()된 문자열도 동일한 패턴을 보인다는 점을 확인했다.  
`new String()`으로 만든 힙 객체는 GC되지만, String Pool의 객체는 클래스 로더가 언로드되지 않는 한 GC되지 않았다.

- `"Literal"` → GC되지 않음
- `intern()` 반환값 → GC되지 않음
- `new String()` 및 intern 호출 전 힙 객체 → GC됨


#### 진짜 이유

**리터럴 String, intern된 String, new String()의 저장 위치**
   
 - 리터럴 String -> 클래스 로딩 시 `Runtime Constant Pool` -> `String Constant Pool`에 저장
 - `intern()`된 String -> String Pool에 등록되어 리터럴과 동일한 Pool 객체 사용
 - `new String()` → 항상 Heap에 새로운 객체 생성

**리터럴 값의 Runtime Constant Pool**

- 클래스 파일의 리터럴은 Runtime Constant Pool에 기록된다.
- 이 Pool 내부에서 String Pool을 통해 리터럴 및 intern된 문자열이 관리된다.


**Runtime Constant Pool과 String Pool의 강한 참조**

- Runtime Constant Pool이 String Pool의 문자열을 **강한 참조** 한다.
- 이 참조는 클래스 로더 생명주기와 동일하게 유지되며, 클래스가 언로드되기 전까지 GC되지 않는다.
- 반면 Heap 객체는 일반 GC 규칙을 따른다.

> 즉, 리터럴과 intern된 문자열이 GC되지 않는 이유는 Runtime Constant Pool이 String Pool을 강하게 참조하고 있기 때문이다.
>   
> _이는 JVM의 메모리 절약 및 성능 최적화를 위한 설계이다._

---
---

## 결론 : String을 선언했을 때의 동작

### `String str = "hello world";` (리터럴 선언)

- 생성/참조 흐름: 리터럴(풀 객체) → str은 그 풀 객체를 그대로 참조
- 객체 수: 1개(풀에 있는 String 객체)
- GC: 보통 클래스 언로드 전까지는 안 죽는다(해당 리터럴이 들어있는 클래스의 constant pool(런타임 상수풀/클래스 메타)이 그 String을 잡고 있음)

#### 클래스 로딩 시점

컴파일된 `.class` 파일의 Constant Pool에 `"hello world"` 리터럴이 기록되어 있다. 

클래스가 클래스 로더에 의해 처음 로딩될 때, JVM은 이 리터럴을 `Runtime Constant Pool`에 로드한다.

`String Constant Pool`에 동일한 값이 없다면 새로 생성해 등록하고, Runtime Constant Pool이 해당 객체를 **강한 참조**한다.

#### 런타임 시점

변수 `str`은 String Pool에 저장된 동일 객체의 참조를 받는다.   (`str`은 그냥 포인터를 가질 뿐, 새로운 객체를 만들지 않는다.)

이후 `str = null`로 참조를 해제해도, String Pool 객체는 `Runtime Constant Pool`에 의해 강한 참조를 받고 있으므로 GC되지 않는다.

---

### `String str = new String("hello world");` (힙 객체 생성)

- 생성/참조 흐름: 리터럴(풀 객체) 확보 → new로 힙 인스턴스 생성(복사) → str은 힙 인스턴스를 참조
- 객체 수: 2개(풀 객체 1 + 힙 인스턴스 1)
- GC: 힙 인스턴스는 참조 끊기면 GC 대상, 풀 객체는 위(1)과 동일하게(리터럴이면 클래스 언로드 전까지 보통 유지)

#### 클래스 로딩 시점

- 위와 동일하게 `"hello world"` 리터럴이 이미` Runtime Constant Pool` 및 `String Constant Pool`에 존재한다.
> (존재하지 않으면 위 1)의 과정을 거쳐 추가된다.)

#### 런타임 시점

- `new String()`이 실행되면 JVM은 Heap에 새로운 String 객체를 생성한다. 
- 변수 `str`은 이 힙 객체를 참조하며, String Pool의 객체와는 별개이다.

#### GC 관점

- 이 힙 객체는 일반 객체처럼 참조가 끊어지면 GC 대상이 된다.
- 단, String Pool에 있는 `"hello world"` 객체는 여전히 Runtime Constant Pool이 참조하므로 GC되지 않는다.

### `intern()`
동작 흐름: “풀에 같은 값 있으면 그 풀 객체 반환 / 없으면 풀에 등록 후 그 객체 반환”

#### GC관점

- JDK 7+에선 String Pool이 힙에 있고, 클래스 리터럴로 잡히는 값이 아니면(=어떤 클래스 constant pool도 안 잡으면) 참조가 완전히 끊겼을 때 풀에 등록된 문자열도 GC될 수 있다.
- 다만 테스트의 "Intern"은 코드에 리터럴로 존재해서(클래스 constant pool에 있음) pooled가 살아남은 것임.

리터럴이든 intern()이든 “풀(String Pool/StringTable)에 있는 객체”를 공유한다는 점은 같고, new String()만 힙에 별도 인스턴스를 하나 더 만드는 거임

--- 

### 이렇게 한다면 ? : `intern()`을 반복적으로 호출하는 경우

```java
public static void main(String[] args) {
    String s = "a";
    for (int i = 0; i < 1_000_000; i++) {
        s = ("" + i).intern();
    }
}
```

"a"는 클래스 로딩 시점에 Constant Pool → String Pool에 등록된다. _(클래스 언로드 전까지 GC되지 않는다.)_

("" + i).intern()은 매 반복마다 새로운 문자열을 만들고, String Pool에 등록하려 시도한다.

JDK 7 이상에서는 String Pool이 Heap으로 옮겨졌기 때문에, 더 이상 강제적으로 풀에 모든 값을 유지하지 않고, 클래스 로더가 참조하지 않는 intern된 문자열은 GC될 수 있다.

따라서 "a"만 GC에서 제외되고, 나머지 1,000,000개의 문자열은 참조가 끊기면 Heap에 있는 Pool 영역에서 GC된다.

---

## 요약
`new String("x")`: 리터럴(풀 객체) → 힙 인스턴스(new로 별도 생성) (풀 객체와 힙 인스턴스 2개)

`"x"`: 리터럴(풀 객체) (풀 객체 1개 공유)

`intern()`: 결과는 “풀 객체”를 돌려준다(리터럴로 잡히면 오래 살고, 아니면 GC될 수도 있음)

| 구분 | 저장 위치                                | 생성 시점 | GC 여부 | 주요 특징 |
|------|--------------------------------------|-----------|---------|-----------|
| `String str = "hello world";` | Runtime Constant Pool -> String Pool | 클래스 로딩 시 | 클래스 언로드 전까지 GC되지 않음 | 동일한 리터럴 공유 |
| `String str = new String("hello world");` | Heap                                 | 런타임 시 매번 생성 | 참조 해제 시 GC됨 | Pool 객체와 별개로 독립 생성 |

---
