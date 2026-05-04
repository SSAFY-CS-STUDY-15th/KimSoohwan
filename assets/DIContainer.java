package container;

import annotation.Singleton;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.lang.reflect.Constructor;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;


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
