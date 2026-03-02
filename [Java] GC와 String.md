# "hello world!"의 심오한 이야기

## JVM 메모리 구조

Java 프로그램이 실행되면 JVM은 데이터를 용도에 따라 여러 영역에 나누어 관리한다.
대표적으로 Heap, Stack, Method Area 같은 영역이 존재한다.

- Heap: new로 생성한 일반 객체가 저장되는 영역

- Stack: 메서드 호출 정보, 지역 변수, 참조값 등이 저장되는 영역

- Method Area: 클래스 메타데이터, static 변수, 메서드 정보, 그리고 상수 풀 같은 클래스 수준 정보를 관리하는 영역

이 중 이번 글에서 중요한 건 Method Area다.
String 리터럴이 왜 일반 객체와 다르게 보이는지 이해하려면, 먼저 이 영역 안에서 관리되는 Runtime Constant Pool을 알아야 한다.

![jvm runtime data areas](./assets/image.png)

---

## Method Area

Method Area는 JVM이 클래스 단위의 정보를 저장하는 논리적 메모리 영역이다.
여기에는 다음과 같은 정보가 들어간다.

- 클래스/인터페이스의 이름, 부모 클래스 정보
- 필드와 메서드 정보
- 바이트코드
- static 변수
- Runtime Constant Pool

즉, Heap이 “객체 인스턴스”를 위한 공간이라면, Method Area는 “클래스 자체에 대한 정보”를 위한 공간이라고 볼 수 있다.

다만 주의할 점은, Method Area는 JVM 명세상의 개념이고, 실제 HotSpot JVM에서는 이를 구현하기 위해 과거에는 PermGen(8이전), 현재는 Metaspace(8이후) 같은 방식이 사용된다.

즉, 여기서 말하는 Method Area는 “이런 역할을 하는 논리적 영역”으로 이해하면 된다.

## Runtime Constant Pool

자바 코드를 컴파일하면 `.class` 파일 안에는 Constant Pool이라는 테이블이 들어간다.
여기에는 클래스가 실행되는 데 필요한 각종 상수 정보들이 저장된다.

예를 들면:

- 문자열 리터럴
- 숫자 리터럴
- 클래스/메서드/필드에 대한 심볼릭 참조

클래스가 JVM에 로딩되면, 이 `.class` 파일의 Constant Pool 정보가 JVM 내부의 Runtime Constant Pool 형태로 올라온다.

> class 파일 안의 Constant Pool = 정적 정보
> JVM에 로딩된 뒤의 Runtime Constant Pool = 런타임에서 사용하는 상수 정보
라고 이해하면 된다.

여기서 중요한 점은, 문자열 리터럴도 이 Constant Pool과 연결되어 관리된다는 것이다.
그래서 String은 단순히 Heap에만 있는 일반 객체처럼 보면 헷갈리기 시작한다.

---

## String 리터럴은 왜 헷갈릴까?

보통 객체는 new로 만들고, 참조가 끊기면 GC 대상이 된다.
그런데 String은 다음처럼 생성 방식이 나뉜다.

- `"hello"` → 리터럴
- `new String("hello")` → 힙에 새 객체 생성
- `"hello".intern()` 혹은 `someString.intern()` → 풀에 있는 canonical 객체 사용
(_canonical 객체(정규 객체): 특정 값을 대표하는 고유한 인스턴스를 의미. 메모리 절약과 비교 연산(==) 효율화를 위해 사용_)

특히 리터럴 문자열은 클래스의 상수 정보와 연결되어 동작하기 때문에, 일반적인 힙 객체와는 생명주기가 다르게 보일 수 있다.

그래서 자연스럽게 이런 의문이 생긴다.

## 리터럴 String은 GC가 안되는데?

Java에서 String은 일반 객체와 비슷하면서도 조금 다르게 보인다.
특히 `"Literal"`처럼 리터럴로 만든 문자열, `new String()`으로 만든 문자열, `intern()`한 문자열이 어디에 있고, 왜 어떤 것은 GC되고 어떤 것은 안 되는지가 헷갈리기 쉽다.

이 글에서는 `WeakReference`를 이용한 간단한 실험으로 세 경우를 비교하고, 그 결과를 바탕으로 Runtime Constant Pool, String Pool, GC 동작의 관계를 정리한다. 참고로 JVM 명세상 Method Area에는 클래스별 메타데이터와 run-time constant pool이 저장되며, 이 영역의 실제 구현 위치나 관리 방식은 JVM 구현체가 결정한다.

---

## 들어가기 전에..

Java 명세는 문자열 리터럴과 문자열 상수식은 intern된다고 설명한다. 즉, `"hello"` 같은 리터럴은 공유 가능한 canonical한 String 인스턴스로 취급된다. 또한 `String.intern()`은 이미 풀에 같은 값이 있으면 그 객체를 반환하고, 없으면 현재 문자열을 풀에 넣고 그 참조를 반환한다. 반면 `new String(original)`은 같은 내용을 가지는 새로운 String 객체를 별도로 생성한다.

### canonical 객체란?

canonical 객체는 “동일한 값을 대표하는 기준 객체” 정도로 이해하면 된다.

예를 들어 문자열 "hello"가 여러 번 등장하더라도, JVM이 매번 새로운 객체를 만드는 대신
"hello"라는 값을 대표하는 하나의 객체를 정해 공유하면 메모리를 아낄 수 있다. 이때 그 대표 객체가 바로 canonical 객체다.

- 값은 같지만 객체는 여러 개일 수도 있다.
- 필요하다면 JVM은 그중 하나를 대표 객체로 삼아 재사용할 수 있다.

String Pool이 바로 이런 canonical 객체를 관리하는 대표적인 예시다

예를 들어:

```java
String a = "hello";
String b = "hello";
```

이 경우 a와 b는 서로 다른 문자열처럼 보이지만, 실제로는 같은 리터럴 값을 대표하는 하나의 canonical 객체를 함께 참조한다.

```java
String a = new String("hello");
String b = new String("hello");
```

이 경우 a, b는 내용은 같아도 각각 별도의 객체다.
즉, 값은 같지만 canonical 객체를 직접 참조한 것이 아니라 독립적으로 생성된 객체들이다.

그리고 `intern()`은 바로 이 지점에서 동작한다.

```java
String s1 = new String("hello");
String s2 = s1.intern();
```

여기서 s2는 "hello"라는 값을 대표하는 canonical 객체를 참조한다.
풀에 이미 "hello"의 대표 객체가 있으면 그것을 돌려주고, 없으면 현재 객체를 대표 객체로 등록한 뒤 그 참조를 반환한다.

String 말고도 대표 객체를 재사용하는 케이스가 더 있다.
Integer 캐시: -128 ~ 127

```java
Integer a = 127;
Integer b = 127;
System.out.println(a == b); // true

Integer c = 128;
Integer d = 128;
System.out.println(c == d); // false
```

---

## 테스트 해보기

이번 실험은 다음 네 가지를 비교한다.

리터럴 문자열

- `new String()`으로 만든 힙 객체
- `intern()` 호출 전의 일반 문자열 객체
- `intern()`이 반환한 풀 객체

실험 방식은 단순하다. 각 문자열을 `WeakReference`로 감싼 뒤 강한 참조를 모두 끊고 GC를 유도해서, 어떤 객체가 남아 있는지 확인한다. 다만 System.gc()는 어디까지나 GC를 요청하는 것일 뿐, 반드시 특정 객체를 수거한다고 보장하지는 않는다. 또 `WeakReference`는 객체가 truly weakly reachable이라고 판단될 때 비로소 클리어된다. 따라서 이 실험은 경향을 확인하는 용도로 보는 편이 맞다.

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

### 출력결과

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

---

## 이 결과가 보여주는 것

이 실험에서 가장 먼저 보이는 사실은 `new String()`으로 만든 객체는 일반 힙 객체처럼 동작한다는 점이다.
heap과 intern 변수는 둘 다 `new String(...)`으로 만들어졌기 때문에, 강한 참조를 끊으면 GC 대상이 된다. 이는 String(String original) 생성자가 기존 문자열과 같은 내용을 가진 새로운 객체를 만든다는 API 설명과도 맞아떨어진다.

반면 `"Literal"`과 `intern()`의 반환값은 살아남았다. 여기서 중요한 포인트는 “intern되었기 때문에 안 죽는다”가 아니라, 그 값이 리터럴로도 존재하기 때문에 살아남기 쉬운 조건이었다는 것이다. Java 명세는 문자열 리터럴이 항상 같은 String 인스턴스를 가리키며 intern된다고 설명하고, JVM 명세는 *ldc가 run-time constant pool에 있는 문자열 리터럴 인스턴스의 참조를 스택에 올릴 수 있다고 설명한다. 즉, 리터럴은 단순히 “풀에 있다”를 넘어 클래스 메타데이터와 연결된 문자열로 이해하는 편이 더 정확하다.

(*ldc:_상수 풀(Constant Pool)에서 지정된 인덱스의 상수(문자열,정수,부동소수점 클래스 참조 등)를 찾아 오퍼랜드 스택(Operand Stack)에 푸시(push)하는 바이트코드. 리터럴 데이터를 메모리에 로딩하거나 String 객체를 생성할 때 주로 사용_)

---

### 많이 헷갈리는 지점: Runtime Constant Pool이 String Pool을 강하게 참조하나?

초반에는 보통 이렇게 이해하기 쉽고, 나또한 그랬다.

> 리터럴 문자열은 Runtime Constant Pool에 있고, Runtime Constant Pool이 String Pool을 강하게 잡고 있어서 GC가 안 된다.

이 설명은 완전히 틀렸다고 보긴 어렵지만, 너무 단순해서 오해를 부른다.

정확히 하면,
- 소스 코드의 `"hello"`는 컴파일되면 class 파일 constant pool에 문자열 상수로 기록된다.
- 클래스가 로드되면 그것은 그 클래스의 Runtime Constant Pool 항목이 된다.
- 바이트코드 ldc가 그 항목을 읽을 때, JVM은 그 문자열을 나타내는 String 객체 참조를 스택에 올린다. 
- JVM 명세는 run-time constant pool entry가 string constant이면, 그 String 인스턴스의 참조를 푸시한다고 설명한다.
- 그런데 문자열 리터럴은 원래부터 intern 대상이므로, 그 참조는 결국 String Pool의 canonical 객체를 가리키게 되는 것이다.

예제로 보면 더 쉽다.

```java
String a = "hello";
```
이 경우 `"hello"`는
클래스의 Runtime Constant Pool 항목으로 존재하고, 실행 시 그 항목을 통해 얻는 String 참조는 String Pool의 canonical 객체다.

반면,
```java
String b = new String("hello");
```
여기서는 `"hello"` 리터럴 자체는 위와 똑같이 Runtime Constant Pool/String Pool과 연결되지만, new `String(...)`은 같은 내용을 가진 새 String 객체를 또 하나 만든다. 

즉:
- `"hello"` 리터럴용 canonical 객체 1개
- `new`로 만든 힙 객체 1개
가 생긴다.

String 생성자는 복사된 새 문자열 객체를 만든다고 문서에 적혀 있다.


```java
String s = someDynamicString.intern();
```
이건 Runtime Constant Pool과 직접 관련이 없는 경우가 많다.
왜냐하면 동적으로 만든 문자열은 애초에 어떤 클래스의 리터럴 항목이 아닐 수 있기 때문이다. 이 경우엔 class의 Runtime Constant Pool에 새 문자열 항목이 생기는 게 아니라, 그냥 String Pool에 canonical 객체가 등록/재사용되는 것이다. `intern()` 문서도 “같은 값이 풀에 있으면 그걸 반환하고, 없으면 현재 객체를 풀에 넣고 그 참조를 반환한다”고 설명한다.

---

### 그럼 intern된 문자열은 무조건 안 죽는가?

여기서 중요한 반전이 있다. `String.intern()`의 명세는 풀의 동작 방식만 설명하지, “한 번 intern되면 절대 GC되지 않는다”까지 보장하지는 않는다. 실제 OpenJDK HotSpot 쪽 구현 이슈를 보면, 현재 HotSpot의 StringTable은 weak reference를 사용한다. 즉, 구현체 관점에서는 인턴되었다는 사실만으로 immortal(_불사_) 객체가 되는 것은 아니다.

이 말은 곧, 리터럴이 아닌 동적 문자열을 `intern()`한 경우에는, 다른 곳에서 강하게 잡고 있지 않다면 나중에 GC될 수 있다는 뜻이다. 반대로 이번 실험의 "Intern"처럼 코드에 리터럴로 직접 등장한 값은, intern 반환값이 그 리터럴 객체와 사실상 같은 객체이기 때문에 살아남는 것이다. 

---

### 왜 리터럴은 대체로 오래 살아남을까?

클래스에 들어 있는 문자열 리터럴은 그 클래스를 통해 다시 접근될 수 있어야 한다. JVM 명세에서도 ldc가 run-time constant pool의 문자열 리터럴 참조를 꺼내 쓸 수 있다고 정의한다. 그래서 일반적인 애플리케이션 코드에서, 특히 애플리케이션 클래스 로더나 부트스트랩 클래스 로더로 로드된 클래스의 리터럴 문자열은 프로그램 실행 내내 살아 있는 경우가 많다.

다만 이것도 “절대 GC되지 않는다”는 뜻은 아니다. Java 명세는 클래스는 그 defining class loader가 GC 가능할 때만 unload될 수 있다고 설명하고, bootstrap loader가 로드한 클래스는 unload되지 않는다고 명시한다. 따라서 리터럴 문자열도 결국은 해당 클래스를 누가 로드했고, 그 클래스 로더가 살아 있는지에 영향을 받는다. 보통 우리가 작성한 일반 애플리케이션에서는 클래스 로더가 오래 살아 있기 때문에 리터럴 문자열도 오래 남아 보이는 것이다.

---

## "hello world" 동작 원리

### `String str = "hello world!";` 의 실제 동작

```java
String str = "hello world!";
```

이 코드는 새로운 문자열 객체를 매번 만드는 코드가 아니다.
문자열 리터럴 `"hello world"`는 intern 대상이고, str은 그 canonical한 String 객체를 참조한다. 즉, 보통은 공유되는 풀 객체 1개만 있다고 보면 된다. Java 명세는 같은 리터럴이 항상 같은 String 인스턴스를 가리킨다고 설명한다.

### String str = new String("hello world"); 의 실제 동작

```java
String str = new String("hello world!");
```

이 경우에는 상황이 다르다.

1. `"hello world!"` 리터럴 자체는 이미 intern된 풀 객체다.
2. 그 다음 `new String(...)`이 실행되면서 같은 내용을 가진 새로운 String 객체가 하나 더 만들어진다.
3. 변수 `str`은 이 새 객체를 참조한다.

즉, 보통은 풀 객체 1개 + 새 힙 객체 1개, 총 2개가 관찰된다.
참조를 끊으면 `new String(...)`로 만든 객체는 일반 힙 객체처럼 GC 대상이 되지만, 리터럴 객체 쪽은 앞서 말한 이유로 계속 살아남을 수 있다.

### `intern()의 실제 의미

```java
String s1 = new String("abc");
String s2 = s1.intern();
```

intern()은 “문자열 값을 canonical한 하나의 객체로 대표시키는 작업”이다.
동작은 단순하다.

- 풀에 같은 값이 있으면 그 객체를 반환
- 없으면 현재 객체를 풀에 넣고 그 참조를 반환

그래서 `intern()`의 반환값은 항상 풀 기준의 대표 객체다.
다만 그 객체가 얼마나 오래 사는지는 별개의 문제다. 리터럴과 연결된 문자열이면 오래 남기 쉽고, 리터럴과 무관한 동적 문자열이면 JVM 구현과 reachability에 따라 GC될 수 있다. HotSpot 구현에서는 StringTable이 weak reference 기반이라는 점이 이 차이를 설명해 준다.

---

## 이 실험의 한계와 보완 실험

현재 실험은 "Intern"이라는 값이 코드에 리터럴로 직접 들어가 있다는 점 때문에, “리터럴과 무관한 순수 intern 문자열”을 보기에는 조금 아쉽다. 이 경우 `intern()`이 반환한 객체가 살아남는 이유에 리터럴 효과가 섞여 있기 때문이다.

### 실험해보자

```java
String dynamic = new StringBuilder()
            .append("dyn-")
            .append(System.nanoTime())
            .toString();

String pooled = dynamic.intern();

NamedWeakRef dynamicRef = new NamedWeakRef("동적 문자열 원본", dynamic, refQ);
NamedWeakRef pooledRef = new NamedWeakRef("동적 문자열 intern() 결과", pooled, refQ);

System.out.println("동적 문자열 값: " + pooled);
System.out.println("dynamic == pooled ? " + (dynamic == pooled));

printAlive("GC 전", dynamicRef, pooledRef);

dynamic = null;
pooled = null;

forceGC();

printAlive("GC 후", dynamicRef, pooledRef);
drainReferenceQueue();
```

### 출력

```text
동적 문자열 값: dyn-108925875514583
dynamic == pooled ? true
GC 전: 동적 문자열 원본 살아있니~? true
GC 전: 동적 문자열 intern() 결과 살아있니~? true
GC 후: 동적 문자열 원본 살아있니~? false
GC 후: 동적 문자열 intern() 결과 살아있니~? false
ReferenceQueue 감지: [동적 문자열 intern() 결과] GC됨
ReferenceQueue 감지: [동적 문자열 원본] GC됨
```

---

### 해석

보완 실험에서는 문자열을 소스 코드에 리터럴로 직접 박아두지 않고, StringBuilder와 `System.nanoTime()`을 이용해 런타임에만 만들어지는 동적 문자열을 생성했다. 이 점이 중요하다. Java 명세에서 문자열 리터럴은 intern되며, JVM은 클래스의 run-time constant pool을 통해 리터럴 문자열 인스턴스를 참조할 수 있다. 하지만 이번에 만들어진 최종 문자열 값 dyn-108925875514583 자체는 클래스 파일에 리터럴로 존재하는 값이 아니므로, 리터럴 문자열처럼 클래스 메타데이터에 의해 붙잡히는 케이스가 아니다.

#### `dynamic == pooled ? true`

출력에서 가장 먼저 눈에 띄는 건 `dynamic == pooled ? true`라는 결과다. 이는 `intern()` 호출 시 동일한 값의 문자열이 풀에 없었기 때문에, 현재 dynamic이 가리키던 그 문자열 자체가 canonical한 풀 객체로 사용되었음을 의미한다. `String.intern()`의 공식 문서에 설명도 풀에 같은 값이 없으면 이 String 객체를 풀에 추가하고 그 참조를 반환한다고 되어 있으므로, 이번 결과는 dynamic과 pooled가 서로 다른 두 객체가 아니라 같은 객체를 가리켰다는 뜻이다.

#### 동적 문자열의 생존여부

```text
GC 후: 동적 문자열 원본 살아있나? false
GC 후: 동적 문자열 intern() 결과 살아있나? false
```

이 결과는 더 중요한 의미를 가진다.
두 참조가 애초에 같은 객체를 가리켰기 때문에, 강한 참조를 모두 null로 끊은 뒤에는 그 객체를 붙잡고 있는 것이 사실상 `WeakReference`뿐이었다. `WeakReference`는 GC의 루트가 아니므로, 객체가 더 이상 강하게 도달 가능하지 않으면 GC 대상이 된다. 그래서 원본도 사라지고, `intern()` 결과도 함께 사라진 것이다. 즉, **“intern된 문자열도 반드시 살아남는 것은 아니다”**라는 점을 이 실험이 직접 보여준다.

이 결과는 이전 실험과도 정확히 대비된다. 이전 실험의 "Intern"은 코드에 문자열 리터럴로 직접 등장했기 때문에, `intern()`이 반환한 객체가 리터럴과 연결된 canonical 객체로 남아 있었다. 반면 보완 실험의 최종 문자열 값은 클래스 파일의 문자열 리터럴로 존재하지 않았기 때문에, `intern()` 이후에도 별도의 강한 루트가 생기지 않았다. 그래서 `intern()`을 했음에도 불구하고, 참조가 완전히 끊기자 GC될 수 있었다.

즉, 이번 실험을 통해 내릴 수 있는 결론은 단순하다.

> intern()은 문자열을 살려두는 기능이 아니라, 같은 값을 대표하는 canonical 객체를 공유하게 만드는 기능이다.

리터럴 문자열은 클래스의 constant pool과 연결되어 있어서 오래 살아남는 것처럼 보이지만, 리터럴과 무관한 동적 문자열을 `intern()`한 경우에는, 그 객체 역시 더 이상 강하게 참조되지 않으면 GC될 수 있다. 특히 HotSpot 구현에서는 StringTable이 weak reference 기반으로 관리된다는 점이 이 현상을 잘 설명해 준다

---

## TL;DR

- String도 결국 객체지만, 생성 방식에 따라 생존 방식이 달라 보인다
- `"hello"` 같은 리터럴 문자열은 클래스의 상수 정보와 연결되어 있어서 보통 오래 살아남는다
- `new String("hello")`는 항상 힙에 별도 객체를 하나 더 만든다
- `intern()`은 문자열을 살려두는 기능이 아니라, 같은 값을 대표하는 `canonical 객체`를 공유하게 만드는 기능이다

따라서 intern된 문자열도 무조건 GC되지 않는 것은 아니다

특히 리터럴과 무관한 동적 문자열을 intern()한 경우에는, 다른 강한 참조가 없으면 GC될 수 있다

즉, 처음 접근한
“리터럴 String은 특수한 영역에 있어서 무조건 안 죽고, intern된 문자열도 안 죽는다”
라는 이해는 정확하지 않다

정확히 말하면,

> 리터럴 문자열은 클래스 메타데이터와 연결되어 오래 살아남기 쉽고, new String()은 일반 힙 객체처럼 동작하며, intern()은 객체를 “대표 객체를 공유”하게 만드는 동작이다.

### GPT가 말해주는 실무 포인트

- 불필요한 new String()을 만들지 말 것
- 문자열 비교는 항상 equals()를 기본으로
- intern()은 반복도가 높은 값에만 신중히 검토
  - 상태값(`READY, RUNNING, FAILED`)처럼 종류가 적고 반복이 많은가?
  - 설정값, 코드값처럼 애플리케이션 전역에서 반복 재사용되는가?
- 로그, 캐시, 파싱 코드에서 문자열 생성 비용을 확인할 것
  - 디버그 로그를 위해 불필요한 문자열 조합 수행
  - key 조합을 매번 새로운 문자열로 만드는 캐시 코드
  - 파싱/정규화 과정에서 중간 문자열 객체를 과도하게 생성하는 코드
- 메모리 문제는 String 자체보다 참조를 붙잡고 있는 구조를 먼저 의심하기
  - `static Map`, `static List`에 문자열을 계속 쌓는 경우
  - 캐시 eviction 없이 key/value가 누적되는 경우
  - 세션, 로컬 캐시, 전역 레지스트리에 사용자 입력 문자열을 계속 보관하는 경우
  - 클래스 로더가 내려가지 않아 관련 메타데이터와 리터럴이 오래 유지되는 경우

#### 참고
https://docs.oracle.com/javase/specs/jvms/se8/
https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/String.html
https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/String.html
https://docs.oracle.com/cd/E19651-01/817-2157-10/pt_tuningjava.html
https://stackoverflow.com/questions/72762061/java-weakreference-is-still-holding-valid-reference-when-referent-is-no-longer-v