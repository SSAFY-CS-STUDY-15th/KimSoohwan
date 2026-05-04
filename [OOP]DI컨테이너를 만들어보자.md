# 왜 밖에서 주입시키죠?

톰캣처럼 동작하는 서블릿 기반 WAS를 구현해보면서, 자연스럽게 내부 구조에 대한 의문이 들기 시작했다. 톰캣은 잘 만들어진 소프트웨어고, 실제 현업에서도 수많은 트래픽을 안정적으로 처리한다. 그런데 구조를 따라 만들다 보니, 이런 생각이 들었다.

`"톰캣은 내부 의존성을 전부 직접 생성하면서 관리했을까?"`

직접 코드를 따라가보니, 생각보다 답은 단순했다. 톰캣은 꽤 많은 부분을 명시적으로 연결한다. 필요한 객체를 만들고, 필요한 곳에 넘기고, 역할을 나눈다. 

그래서 오히려 반대로 궁금해졌다.

- 왜 톰캣은 DI 컨테이너를 기본으로 두지 않았을까?
- 나는 왜 구현하면서 DI 컨테이너가 필요하다고 느꼈을까?
- 그리고 이런 고민 끝에 왜 Spring 같은 프레임워크가 등장했을까?

이번 글은 그 흐름을 정리한 기록이다.

## SOLID는 왜 나왔을까

객체지향이 널리 쓰이기 시작한 뒤, 코드가 클래스 단위로 나뉜다고 해서 자동으로 유지보수가 쉬워지는 건 아니라는 문제가 금방 드러났다. 클래스는 많아졌는데 수정 한 번 하려면 여기저기 같이 바뀌고, 테스트는 어렵고, 상위 모듈이 하위 구현에 강하게 묶여서 변경 비용이 커졌다.

> 의존성을 어떤 방향으로 둘 것인가

이 맥락에서 정리된 대표적인 기준이 SOLID다.

- SRP: 한 클래스는 한 가지 이유로만 변경되어야 한다
- OCP: 기존 코드를 크게 흔들지 않고 기능을 확장할 수 있어야 한다
- LSP: 상위 타입 자리에 하위 타입을 넣어도 동작 의미가 깨지면 안 된다
- ISP: 사용하지도 않는 큰 인터페이스에 의존하지 않도록 쪼개야 한다
- DIP: 구체 구현이 아니라 추상에 의존하도록 설계해야 한다

DI 컨테이너 구현에서 크게 느낀 건 SRP와 DIP였다.

예를 들어 어떤 클래스가 자기 역할도 수행하고, 동시에 필요한 의존 객체를 직접 생성까지 하고 있으면 책임이 섞인다. 그리고 그 생성 코드 안에는 구체 구현이 박혀버린다. 그러면 테스트도 불편하고, 확장도 불편하고, 교체도 어렵다.

SOLID는 규모가 커질수록 생기는 유지보수 문제를 줄이기 위해 나온 실전 규칙에 가깝다고 생각한다.

## IoC / DI는 뭔가

이 지점에서 IoC와 DI가 등장한다.

IoC(Inversion of Control)는 제어의 흐름을 객체 자신이 아니라 외부가 잡는다는 이야기다. 예전에는 클래스가 내부에서 `new`를 호출해 필요한 객체를 직접 만들었다면, IoC 환경에서는 객체가 필요한 것을 "선언"하고 실제 생성과 연결은 바깥이 담당한다.

DI(Dependency Injection)는 그 IoC를 구현하는 대표적인 방법이다. 필요한 의존성을 생성자나 세터를 통해 주입한다.

예전 방식은 대체로 이런 느낌이다.

```java
public class UserFacade {
    private final UserDao userDao = new UserDao();
    private final EncryptUtil encryptUtil = new EncryptUtil();
}
```

DI 방식은 이런 쪽에 가깝다.

```java
public class UserFacade {
    private final UserDao userDao;
    private final EncryptUtil encryptUtil;

    public UserFacade(UserDao userDao, EncryptUtil encryptUtil) {
        this.userDao = userDao;
        this.encryptUtil = encryptUtil;
    }
}
```

차이는 단순히 코드 취향이 아니다.

- 객체가 "무엇이 필요한지"만 말하게 된다
- 생성 책임이 외부로 빠진다
- 테스트에서 가짜 구현을 넣기 쉬워진다
- 의존 관계를 한곳에서 조립할 수 있다

즉 DI는 편의 기능이면서 동시에 설계의 방향을 바꾸는 도구다.

## 톰캣엔 왜 DI 컨테이너 같은 게 없을까

처음에는 좀 의외였다. 그런데 생각해보면 톰캣의 역할을 보면 이해가 된다.

톰캣은 어디까지나 Servlet 스펙을 구현한 WAS다. HTTP 요청을 받고, 소켓을 관리하고, 스레드를 돌리고, 서블릿 생명주기를 관리하고, 요청을 적절한 서블릿으로 전달하는 일에 집중한다. 즉 "플랫폼" 역할이 핵심이다.

여기서 애플리케이션 내부 객체까지 스스로 조립하기 시작하면, 톰캣은 단순한 WAS가 아니라 프레임워크가 되어버린다. 그러면 범용성도 줄고, 가벼움도 줄고, 책임 범위도 커진다.

그래서 톰캣은 대체로 이런 철학에 가깝다고 느꼈다.

- 플랫폼은 가볍고 범용적이어야 한다
- 비즈니스 컴포넌트 관리까지 WAS가 떠안지 않는다
- 필요한 경우 Spring 같은 상위 프레임워크가 그 위에서 조립을 맡는다

실제로 Spring MVC도 Tomcat 위에서 돌아간다. 요청을 받는 역할은 Tomcat이 하고, 컨트롤러/서비스/리포지토리 같은 객체의 생성과 wiring은 Spring이 맡는다. 역할 분리가 꽤 명확하다.

즉, DI가 없어서 톰캣이 부족한 게 아니라, 애초에 그건 톰캣의 책임이 아니었다고 보는 편이 맞다.

## 그런데 나는 왜 DI 컨테이너가 필요하다고 느꼈을까

문제는 "톰캣의 철학"과 "내가 구현하면서 느낀 불편"이 정확히 같지는 않았다는 점이다.

혼자 WAS를 설계하다 보니, 생각보다 내부 컴포넌트들이 금방 서로 얽히기 시작했다.

- `Dispatcher`는 `StaticServlet`, `AppServlet`에 의존한다
- `FilterConfig`는 여러 필터 객체를 의존한다
- 각 필터와 파사드, DAO도 다시 다른 객체를 의존한다
- 싱글톤으로 관리하고 싶은 객체도 생긴다

규모가 작을 때는 그냥 `new`로 묶으면 된다. 그런데 조금만 커져도 불편이 바로 생긴다.

- 어디서 어떤 객체를 생성하는지 추적이 어렵다
- 생성 순서를 사람이 계속 신경써야 한다
- 테스트에서 일부 구현만 바꾸기가 불편하다
- 싱글톤 보장을 수동으로 맞추기가 번거롭다

그래서 결국 이런 생각이 들었다.

`이 부분은 Spring을 따라서, 아주 단순한 수준으로라도 의존성을 주입해보자.`

## 내가 구운 DI 컨테이너

- `@Singleton`이 붙은 클래스는 컨테이너가 하나만 생성한다
- 생성자의 파라미터 타입을 읽어서 필요한 의존 객체를 재귀적으로 생성한다
- 애플리케이션 시작 시 `initialize()`로 주요 싱글톤을 미리 올린다

핵심 흐름은 아래와 같다.

```java
public static <T> T getInstance(Class<T> clazz) {
    if (clazz.isAnnotationPresent(Singleton.class)) {
        Object existing = singletonInstances.get(clazz);
        if (existing != null && existing != PLACEHOLDER) {
            return (T) existing;
        }

        if (underConstruction.get().putIfAbsent(clazz, true) != null) {
            return (T) existing;
        }

        try {
            singletonInstances.put(clazz, PLACEHOLDER);
            T instance = createInstance(clazz);
            singletonInstances.put(clazz, instance);
            return instance;
        } finally {
            underConstruction.get().remove(clazz);
        }
    }

    return createInstance(clazz);
}
```

생성 자체는 생성자 기반 주입이다.

```java
private static <T> T createInstance(Class<T> clazz) {
    Constructor<?> constructor = clazz.getDeclaredConstructors()[0];
    constructor.setAccessible(true);

    Object[] dependencies =
        Arrays.stream(constructor.getParameterTypes())
            .map(DIContainer::getInstance)
            .toArray();

    return (T) constructor.newInstance(dependencies);
}
```

초기 구동 시점에는 필요한 싱글톤을 명시적으로 등록한다.

```java
private static void initializeDependencies() {
    DIContainer.initialize(
        ConnectionManager.class,
        FilterConfig.class,
        Dispatcher.class
    );
}
```

실제로 `Dispatcher` 같은 객체는 생성자만 선언해두면 된다.

<a href = "./assets/Dispatcher.java">Dispatcher</a>

```java
@Singleton
public class Dispatcher {
    private final StaticServlet defaultServlet;
    private final AppServlet appServlet;

    public Dispatcher(StaticServlet staticServlet, AppServlet appServlet) {
        this.defaultServlet = staticServlet;
        this.appServlet = appServlet;
    }
}
```

이 구조는 객체가 자기 의존성을 직접 만들지 않아도 되고, 생성자 시그니처만 봐도 어떤 협력 객체가 필요한지 드러난다. 적어도 코드가 커질수록 `new`가 여기저기 흩어지는 문제는 줄어든다.

## 바로 마주한 문제

만들자마자 나타난 문제가 있다.


```java
java.lang.IllegalStateException: Recursive update
```

초기에는 싱글톤을 `computeIfAbsent()`로 만들고 있었다.

```java
return (T) singletonInstances.computeIfAbsent(clazz, c -> {
    log.info("[DI] 싱글톤 생성 시작: {}", c.getName());
    return createInstance(c);
});
```

어떤 싱글톤을 생성하는 도중 그 싱글톤이 다시 필요해지는 순간이 생길 수 있다. `computeIfAbsent()`는 이런 재진입 상황을 "같은 key에 대한 중복 갱신"으로 보고 바로 예외를 던진다.

의존성 그래프를 따라 들어가며 객체를 조립하는 컨테이너에는 맞지 않았다.

## 해결

해결은 생성 완료 전 placeholder를 먼저 등록하는 방식이었다.

```java
private static final Object PLACEHOLDER = new Object();

if (clazz.isAnnotationPresent(Singleton.class)) {
    Object existing = singletonInstances.get(clazz);
    if (existing != null && existing != PLACEHOLDER) {
        return (T) existing;
    }

    if (underConstruction.get().putIfAbsent(clazz, true) != null) {
        return (T) existing;
    }

    try {
        singletonInstances.put(clazz, PLACEHOLDER);
        T instance = createInstance(clazz);
        singletonInstances.put(clazz, instance);
        return instance;
    } finally {
        underConstruction.get().remove(clazz);
    }
}
```

- 생성 시작 전에 `PLACEHOLDER`를 먼저 넣는다
- `ThreadLocal`로 현재 생성 중인 타입을 추적한다

이렇게 바꾸니 "초기화 중 동일 싱글톤 재요청" 때문에 터지는 문제는 피할 수 있었다.

다만 이건 완전한 해결은 아니다. 지금 구조는 생성 중 객체에 대한 조기 참조를 아주 제한적으로만 흉내 낸다. 프록시도 없고, 생성 도중 참조된 placeholder를 실제 안전한 객체처럼 다룰 수 있는 것도 아니다. 즉, 순환 의존을 본격적으로 지원한다고 말하기는 어렵다.

그래도 적어도 하나는 분명히 배웠다.

`DI 컨테이너에서 고려할 점은 생성 시점과 초기화 상태를 관리하는 것이다.`

## 실제 Spring에서는 어떻게 하나

여기서부터가 꽤 흥미로웠다. 직접 부딪혀보니 왜 Spring이 큰 프레임워크가 되었는지 조금 이해가 갔다.

Spring은 단순히 "리플렉션으로 생성자 호출해주는 도구"가 아니다. 실제로는 다음 역할이 분리되어 있다.

- `BeanDefinition`: 어떤 클래스를 어떤 스코프로 어떤 방식으로 만들지에 대한 메타데이터
- `DefaultListableBeanFactory`: 빈 정의를 보관하고 조회하는 중심 팩토리
- `AbstractAutowireCapableBeanFactory`: 생성자 선택, 의존성 주입, 초기화 수행
- `DefaultSingletonBeanRegistry`: 싱글톤 캐시와 생성 중 상태 관리

내 구현은 `Class<?> -> Object` 중심이었다면, Spring은 `bean name + bean definition + lifecycle` 중심에 가깝다. 생성 규칙을 가진 컨테이너로서 동작한다.

생성 도중 다시 같은 빈이 필요해지는 상황을 Spring은 훨씬 정교하게 다룬다.

Spring의 싱글톤 관리는 개념적으로 이런 식이다.



```java
// 개념 요약
singletonObjects       // 완전히 생성 완료된 객체
earlySingletonObjects  // 생성은 되었지만 초기화가 덜 끝난 조기 참조
singletonFactories     // 조기 참조를 만들어내는 팩토리
```

```java
beforeSingletonCreation(beanName);
Object bean = createBeanInstance(beanName, mbd);

addSingletonFactory(beanName, () -> getEarlyBeanReference(beanName, mbd, bean));

populateBean(beanName, mbd, bean);
initializeBean(beanName, bean, mbd);

addSingleton(beanName, bean);
afterSingletonCreation(beanName);
```

내가 넣은 `PLACEHOLDER` 하나와 비교하면, Spring은 아예 조기 노출용 캐시와 조기 참조를 만드는 팩토리를 별도로 둔다. 그래서 AOP 프록시가 필요한 경우에도, 나중에 바뀔 객체가 아니라 처음부터 노출해야 할 참조 형태를 더 세밀하게 맞출 수 있다.

다만 중요한 점도 있다.

Spring도 생성자 기반 순환 의존은 원칙적으로 해결하지 못한다. 공식 문서에서도 생성자 주입 중심의 순환 참조는 런타임에 감지되어 예외가 난다고 설명한다. 즉 Spring이 만능으로 다 풀어주는 게 아니라, 설계 자체가 강하게 꼬여 있으면 거기서 멈춰 세운다.

### DefaultSingletonBeanRegistry

여기가 스프링 객체 캐시의 중심이다.
```text
singletonObjects
earlySingletonObjects
singletonFactories
```

Spring 소스에서도 이 클래스 안에 각각 “완성된 singleton cache”, “early singleton cache”, “singleton factory registry”로 필드가 잡혀 있다. 즉 순환참조랑 조기 노출 구조를 보려면 여기를 먼저 봐야 한다

<a href="https://github.com/spring-projects/spring-framework/blob/main/spring-beans/src/main/java/org/springframework/beans/factory/support/DefaultSingletonBeanRegistry.java">자세히 보기</a>

주요 메서드

```java
getSingleton(String beanName)
getSingleton(String beanName, boolean allowEarlyReference)
getSingleton(String beanName, ObjectFactory<?> singletonFactory)

addSingletonFactory(...)
addSingleton(...)
beforeSingletonCreation(...)
afterSingletonCreation(...)
```

흐름

```text
singletonObjects 조회
→ 없고 생성 중이면 earlySingletonObjects 조회
→ 그래도 없고 allowEarlyReference면 singletonFactories 조회
→ factory.getObject()
→ earlySingletonObjects에 넣음
→ singletonFactories에서 제거
```

### 빈 생성 흐름

```java
Object bean = createBeanInstance(beanName, mbd);

addSingletonFactory(beanName, () -> getEarlyBeanReference(beanName, mbd, bean));

populateBean(beanName, mbd, bean);
initializeBean(beanName, bean, mbd);
```

doCreateBean()이 bean instance를 만들고, 순환참조 해결을 위해 addSingletonFactory(beanName, () -> getEarlyBeanReference(...))를 등록한 뒤, populateBean()과 initializeBean()을 호출한다.

흐름
```text
createBeanInstance()
→ 객체 생성, 아직 의존성 주입 전

addSingletonFactory()
→ 순환참조 대비용 조기 참조 팩토리 등록

populateBean()
→ 필드/세터 의존성 주입

initializeBean()
→ Aware, BeanPostProcessor, initMethod, 프록시 후처리 등
```

<a href="https://github.com/spring-projects/spring-framework/blob/main/spring-beans/src/main/java/org/springframework/beans/factory/support/AbstractAutowireCapableBeanFactory.java">자세히 보기</a>

### beforeSingletonCreation, afterSingletonCreation

```java
beforeSingletonCreation(beanName);
singletonFactory.getObject();
afterSingletonCreation(beanName);
addSingleton(beanName, singletonObject);
```

실제로 `beforeSingletonCreation()`은 `singletonsCurrentlyInCreation`에 beanName을 추가하고, 이미 생성 중이면 `BeanCurrentlyInCreationException`을 던진다.

`afterSingletonCreation()`은 생성 완료 후 그 beanName을 생성 중 목록에서 제거한다.

beforeSingletonCreation()
→ "이 빈 지금 생성 중임" 표시

afterSingletonCreation()
→ "이제 생성 중 아님" 표시

이걸로 isSingletonCurrentlyInCreation(beanName) 판단이 가능하고, 그 판단으로 조기 참조 노출 여부를 결정.

<a href="https://github.com/spring-projects/spring-framework/blob/main/spring-beans/src/main/java/org/springframework/beans/factory/support/DefaultSingletonBeanRegistry.java">자세히 보기</a>

## 부족한 점

톰캣이 DI 없이도 충분히 동작하는 걸 보면, 내가 굳이 WAS 레벨에서 이걸 넣은 게 과한 선택일 수도 있다. 정말 lightweight하게 가져가려면 명시적 조립이 더 나을 수도 있다. 반대로, 내가 만든 구조처럼 내부 컴포넌트 수가 계속 늘어난다면 생성과 연결을 한 곳에서 관리하는 편이 훨씬 편해질 수도 있다.

아직 빠진 것도 많다.

- 빈 생명주기 관리
- 더 명확한 순환 참조 검증
- 인터페이스 기반 바인딩
- 스캔 기반 자동 등록
- 프록시나 후처리기 같은 확장 포인트

## 배운 점

톰캣은 최소한의 플랫폼으로서의 철학을 따랐고, 나는 개발자의 관점에서 편리한 조립 구조를 원해서 DI를 붙여봤다. 둘 중 하나만 정답이라고 보긴 어렵다. 다만 직접 만들어보니 "왜 Spring이 필요해졌는가", 그리고 "왜 DI 컨테이너가 단순한 편의 기능이 아닌가"는 확실히 이해됐다.

결국 이번 DI 컨테이너는 완성품이라기보다 출발점에 가깝다. 그래도 적어도 이제는, 의존성 주입이 왜 등장했는지와 싱글톤 초기화가 왜 생각보다 어려운지에 대해서는 예전보다 훨씬 선명하게 말할 수 있게 되었다.


### DI container 코드

<a href = "./assets/DIContainer.java">DIContainer</a>

```java
public class DIContainer {

    private static final Logger log = LoggerFactory.getLogger(DIContainer.class);
    private static final Map<Class<?>, Object> singletonInstances = new ConcurrentHashMap<>();
    private static final ThreadLocal<Map<Class<?>, Boolean>> underConstruction =
        ThreadLocal.withInitial(ConcurrentHashMap::new);

    private static final Object PLACEHOLDER = new Object();

    public static <T> T getInstance(Class<T> clazz) {
        try {
            log.info("[DI] getInstance 요청: {}", clazz.getName());

            if (clazz.isAnnotationPresent(Singleton.class)) {
                Object existing = singletonInstances.get(clazz);
                if (existing != null && existing != PLACEHOLDER) {
                    return (T) existing;
                }

                if (underConstruction.get().putIfAbsent(clazz, true) != null) {
                    log.warn("[DI] 순환 의존 감지: {} (생성 중 객체 반환)", clazz.getName());
                    return (T) existing; // 생성 중이면 placeholder 반환 (주의: 프록시 없으니 NPE 가능)
                }

                try {
                    log.info("[DI] 싱글톤 생성 시작: {}", clazz.getName());
                    singletonInstances.put(clazz, PLACEHOLDER);
                    T instance = createInstance(clazz);
                    singletonInstances.put(clazz, instance);
                    log.info("[DI] {} 싱글톤 등록 완료", clazz.getName());
                    return instance;
                } finally {
                    underConstruction.get().remove(clazz);
                }
            }

            return createInstance(clazz);
        } catch (Exception e) {
            log.error("[DI] 인스턴스 조회 실패: {} | 원인: {}", clazz.getName(), e.getMessage(), e);
            throw new RuntimeException(clazz.getName() + "인스턴스 조회 실패", e);
        }
    }

    private static <T> T createInstance(Class<T> clazz) {
        try {
            log.info("[DI] createInstance 호출: {}", clazz.getName());
            Constructor<?> constructor = clazz.getDeclaredConstructors()[0];
            constructor.setAccessible(true);

            Object[] dependencies =
                java.util.Arrays.stream(constructor.getParameterTypes())
                    .peek(dep -> log.info("[DI] {} 생성에 필요한 의존성: {}", clazz.getName(), dep.getName()))
                    .map(DIContainer::getInstance)
                    .toArray();

            T instance = (T) constructor.newInstance(dependencies);
            log.info("[DI] {} 인스턴스 생성 완료", clazz.getName());
            return instance;

        } catch (Exception e) {
            log.error("[DI] 인스턴스 생성 실패: {} | 원인: {}", clazz.getName(), e.getMessage(), e);
            throw new RuntimeException(clazz.getName() + "인스턴스 생성 실패", e);
        }
    }

    public static void initialize(Class<?>... componentClasses) {
        for (Class<?> clazz : componentClasses) {
            if (clazz.isAnnotationPresent(Singleton.class)) {
                getInstance(clazz);
            }
        }
    }
}
```

<a href = "./assets/Singleton.java">Singleton annotation</a>

이 어노테이션을 붙이면 알아서 DI Container 가 읽어서 싱글톤 스코프로 생성해서 의존성 주입함(리플렉션)

```java
@Target(ElementType.TYPE)
@Retention(RetentionPolicy.RUNTIME)
public @interface Singleton {
}
```

사용 예시

```java
@Singleton
public class Dispatcher {
  private final Logger logger = LoggerFactory.getLogger(Dispatcher.class);
  private final StaticServlet defaultServlet;
  private final AppServlet appServlet;

  public Dispatcher(StaticServlet staticServlet, AppServlet appServlet) {
    this.defaultServlet = staticServlet;
    this.appServlet = appServlet;
  }
  //..
```

생성자 만들어두면 필요한 의존성 주입해줌.

부족한점..

<a href ="./assets/WebServer.java">가장 상위 클래스는 직접 등록을 해줘야 함..</a>

```java
public class WebServer {

  public static void main(String args[]) throws Exception {
    startH2Console();

    initializeDependencies();
    ServerConfig config = new ServerConfig();
    config.startServer();
  }

  private static void startH2Console() throws Exception {
    Server.createWebServer("-web", "-webAllowOthers", "-tcpAllowOthers").start();
  }

  private static void initializeDependencies() {
    DIContainer.initialize(
        // connection
        ConnectionManager.class,
        //filter
        FilterConfig.class,
        //router
        Dispatcher.class
    );
  }

}
```
