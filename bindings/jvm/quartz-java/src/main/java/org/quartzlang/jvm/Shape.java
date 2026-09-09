package org.quartzlang.jvm;

import java.lang.annotation.Annotation;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.AccessibleObject;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.RecordComponent;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** How a Java class maps to Quartz values: its components, a constructor and getters as cached method handles. */
final class Shape {
    enum Form {
        /** A Java record, or a Kotlin data class: named components in constructor order. */
        PRODUCT,
        /** A Kotlin {@code object}: one instance, a variant without values. */
        SINGLETON,
        /** A sealed interface or class whose subclasses are the variants. */
        SEALED,
        OTHER
    }

    private static final MethodHandles.Lookup LOOKUP = MethodHandles.lookup();
    private static final MethodType GETTER = MethodType.methodType(Object.class, Object.class);
    private static final ClassValue<Shape> SHAPES = new ClassValue<>() {
        @Override
        protected Shape computeValue(Class<?> type) {
            return new Shape(type);
        }
    };

    final Class<?> type;
    final Form form;
    final String[] names;
    final java.lang.reflect.Type[] types;
    final MethodHandle[] getters;
    final MethodHandle constructor;
    final Object instance;
    final List<Class<?>> subclasses;
    final String problem;
    private final ConcurrentHashMap<Type, int[]> readPlans = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Type, MethodHandle[]> writePlans = new ConcurrentHashMap<>();
    private volatile Map<String, MethodHandle> beanGetters;

    static Shape of(Class<?> type) {
        return SHAPES.get(type);
    }

    private Shape(Class<?> c) {
        type = c;
        Form f = Form.OTHER;
        String[] n = new String[0];
        java.lang.reflect.Type[] t = new java.lang.reflect.Type[0];
        MethodHandle[] g = new MethodHandle[0];
        MethodHandle ctor = null;
        Object single = null;
        List<Class<?>> subs = List.of();
        String why = null;
        try {
            if (c.isRecord()) {
                RecordComponent[] rc = c.getRecordComponents();
                n = new String[rc.length];
                t = new java.lang.reflect.Type[rc.length];
                g = new MethodHandle[rc.length];
                Class<?>[] raw = new Class<?>[rc.length];
                for (int i = 0; i < rc.length; ++i) {
                    n[i] = rc[i].getName();
                    t[i] = rc[i].getGenericType();
                    raw[i] = rc[i].getType();
                    g[i] = getter(rc[i].getAccessor());
                }
                ctor = constructor(c.getDeclaredConstructor(raw));
                f = Form.PRODUCT;
            } else if (isKotlin(c) && c.getSuperclass() != null && hasComponents(c)) {
                List<Method> components = components(c);
                List<Field> fields = new ArrayList<>();
                for (Field field : c.getDeclaredFields()) {
                    if (!Modifier.isStatic(field.getModifiers())) {
                        fields.add(field);
                    }
                }
                int size = components.size();
                n = new String[size];
                t = new java.lang.reflect.Type[size];
                g = new MethodHandle[size];
                Class<?>[] raw = new Class<?>[size];
                for (int i = 0; i < size; ++i) {
                    Method m = components.get(i);
                    // The backing fields of a data class come first, in constructor order: checked against componentN.
                    if (i >= fields.size() || fields.get(i).getType() != m.getReturnType()) {
                        throw new IllegalStateException("cannot match the properties of " + c.getName()
                                + " to its components");
                    }
                    n[i] = fields.get(i).getName();
                    t[i] = fields.get(i).getGenericType();
                    raw[i] = m.getReturnType();
                    g[i] = getter(m);
                }
                ctor = constructor(c.getDeclaredConstructor(raw));
                f = Form.PRODUCT;
            } else if (isKotlin(c) && singleton(c) != null) {
                single = singleton(c);
                f = Form.SINGLETON;
            } else if (c.isSealed() || (isKotlin(c) && Modifier.isAbstract(c.getModifiers()))) {
                subs = subclasses(c);
                f = subs.isEmpty() ? Form.OTHER : Form.SEALED;
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            f = Form.OTHER;
            why = e.getMessage();
        }
        form = f;
        names = n;
        types = t;
        getters = g;
        constructor = ctor;
        instance = single;
        subclasses = subs;
        problem = why;
    }

    /** For each component, the index of the struct field with its name. */
    int[] readPlan(Type struct) {
        return readPlans.computeIfAbsent(struct, s -> {
            int[] plan = new int[names.length];
            for (int j = 0; j < names.length; ++j) {
                plan[j] = fieldIndex(s, names[j]);
                if (plan[j] < 0) {
                    throw new IllegalArgumentException(type.getSimpleName() + "." + names[j]
                            + " has no field of that name in the struct; it has " + fieldNames(s));
                }
            }
            return plan;
        });
    }

    /** For each struct field, the getter that reads it from an instance of this class. */
    MethodHandle[] writePlan(Type struct) {
        return writePlans.computeIfAbsent(struct, s -> {
            List<Type.Field> fields = s.fields();
            MethodHandle[] plan = new MethodHandle[fields.size()];
            for (int i = 0; i < plan.length; ++i) {
                String name = fields.get(i).name();
                plan[i] = form == Form.PRODUCT ? componentGetter(name) : beanGetter(name);
                if (plan[i] == null) {
                    throw new IllegalArgumentException(type.getSimpleName() + " has nothing for the struct field "
                            + name + (problem == null ? "" : " (" + problem + ")"));
                }
            }
            return plan;
        });
    }

    Object construct(Object[] args) throws Throwable {
        return (Object) constructor.invokeExact(args);
    }

    private MethodHandle componentGetter(String name) {
        for (int j = 0; j < names.length; ++j) {
            if (names[j].equals(name)) {
                return getters[j];
            }
        }
        String wanted = Names.normalize(name);
        for (int j = 0; j < names.length; ++j) {
            if (Names.normalize(names[j]).equals(wanted)) {
                return getters[j];
            }
        }
        return null;
    }

    private MethodHandle beanGetter(String name) {
        Map<String, MethodHandle> known = beanGetters;
        if (known == null) {
            known = new HashMap<>();
            for (Field field : type.getFields()) {
                if (!Modifier.isStatic(field.getModifiers())) {
                    try {
                        known.putIfAbsent(Names.normalize(field.getName()),
                                LOOKUP.unreflectGetter(field).asType(GETTER));
                    } catch (IllegalAccessException ignored) {
                        // not readable from here, so not part of the shape
                    }
                }
            }
            for (Method m : type.getMethods()) {
                String key = beanName(m);
                if (key != null) {
                    try {
                        known.put(key, getter(m));
                    } catch (IllegalAccessException ignored) {
                        // not readable from here, so not part of the shape
                    }
                }
            }
            beanGetters = known;
        }
        return known.get(Names.normalize(name));
    }

    private static String beanName(Method m) {
        if (Modifier.isStatic(m.getModifiers()) || m.getParameterCount() != 0 || m.getReturnType() == void.class
                || m.getDeclaringClass() == Object.class) {
            return null;
        }
        String name = m.getName();
        if (name.length() > 3 && name.startsWith("get") && Character.isUpperCase(name.charAt(3))) {
            return Names.normalize(name.substring(3));
        }
        if (name.length() > 2 && name.startsWith("is") && Character.isUpperCase(name.charAt(2))) {
            return Names.normalize(name.substring(2));
        }
        return Names.normalize(name);
    }

    static int fieldIndex(Type struct, String name) {
        List<Type.Field> fields = struct.fields();
        for (int i = 0; i < fields.size(); ++i) {
            if (fields.get(i).name().equals(name)) {
                return i;
            }
        }
        String wanted = Names.normalize(name);
        for (int i = 0; i < fields.size(); ++i) {
            if (Names.normalize(fields.get(i).name()).equals(wanted)) {
                return i;
            }
        }
        return -1;
    }

    private static List<String> fieldNames(Type struct) {
        return struct.fields().stream().map(Type.Field::name).toList();
    }

    private static MethodHandle getter(Method m) throws IllegalAccessException {
        open(m);
        return LOOKUP.unreflect(m).asType(GETTER);
    }

    private static MethodHandle constructor(Constructor<?> c) throws IllegalAccessException {
        open(c);
        MethodHandle h = LOOKUP.unreflectConstructor(c);
        return h.asSpreader(Object[].class, c.getParameterCount())
                .asType(MethodType.methodType(Object.class, Object[].class));
    }

    private static void open(AccessibleObject member) {
        member.trySetAccessible();
    }

    private static boolean isKotlin(Class<?> c) {
        for (Annotation a : c.getDeclaredAnnotations()) {
            if (a.annotationType().getName().equals("kotlin.Metadata")) {
                return true;
            }
        }
        return false;
    }

    private static boolean hasComponents(Class<?> c) {
        try {
            c.getMethod("component1");
            return true;
        } catch (NoSuchMethodException e) {
            return false;
        }
    }

    private static List<Method> components(Class<?> c) {
        List<Method> out = new ArrayList<>();
        for (int i = 1;; ++i) {
            try {
                out.add(c.getMethod("component" + i));
            } catch (NoSuchMethodException e) {
                return out;
            }
        }
    }

    private static Object singleton(Class<?> c) {
        try {
            Field f = c.getField("INSTANCE");
            return Modifier.isStatic(f.getModifiers()) && f.getType() == c ? f.get(null) : null;
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    private static List<Class<?>> subclasses(Class<?> c) {
        Class<?>[] permitted = c.getPermittedSubclasses();
        List<Class<?>> out = new ArrayList<>();
        if (permitted != null) {
            out.addAll(List.of(permitted));
        } else {
            for (Class<?> nested : c.getDeclaredClasses()) {
                if (c.isAssignableFrom(nested) && nested != c) {
                    out.add(nested);
                }
            }
        }
        return List.copyOf(out);
    }
}
