package org.quartzlang.jvm;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

/** Writes Java values into slots; lists, maps and recursive payloads are built on a {@link Walk}, elements first. */
final class SlotWriter {
    /** A list, map or one-element box still to build, and the slot that takes it. */
    private record Job(Type type, Object value, long[] out, int at) {}

    private final Sandbox owner;
    private final Slots.Makers makers;

    private SlotWriter(Sandbox owner, Slots.Makers makers) {
        this.owner = owner;
        this.makers = makers;
    }

    static void encode(Type type, Object value, long[] out, int at, Sandbox owner, Slots.Makers makers)
            throws Throwable {
        SlotWriter w = new SlotWriter(owner, makers);
        List<Job> jobs = w.inline(type, value, out, at, null);
        if (jobs != null) {
            Walk.run(w.new Jobs(jobs));
        }
    }

    private List<Job> inline(Type type, Object value, long[] out, int at, List<Job> jobs) throws Throwable {
        if (value instanceof Value v && Slots.isComposite(type.kind())) {
            Slots.passBackWide(type, v, out, at, owner);
            return jobs;
        }
        switch (type.kind()) {
            case VOID -> {}
            case STRUCT -> jobs = fill(type.fields(), Slots.structValues(type, value), out, at, jobs);
            case TUPLE -> jobs = fill(type.fields(), Slots.tupleValues(type, value), out, at, jobs);
            case ENUM -> {
                Variant v = Slots.variantOf(type, value);
                Type.Case c = type.findCase(v.name());
                if (c == null) {
                    throw new IllegalArgumentException("no variant " + v.name() + " in " + type + "; it has "
                            + type.cases().stream().map(Type.Case::name).toList());
                }
                if (c.payload().size() != v.values().size()) {
                    throw new IllegalArgumentException("variant " + c.name() + " takes " + c.payload().size()
                            + " values, got " + v.values().size());
                }
                Arrays.fill(out, at, at + type.slots(), 0L);
                out[at] = c.index();
                jobs = fill(c.payload(), v.values().toArray(), out, at, jobs);
            }
            case NULLABLE -> {
                Arrays.fill(out, at, at + type.slots(), 0L);
                if (value != null) {
                    if (type.isPointerNullable()) {
                        jobs = inline(type.element(), value, out, at, jobs);
                    } else {
                        out[at] = 1;
                        jobs = inline(type.element(), value, out, at + 1, jobs);
                    }
                }
            }
            case LIST, MAP -> {
                if (value == null || value instanceof Value) {
                    out[at] = Slots.scalar(type, value, owner, makers);
                } else if (type.kind() == Kind.LIST && value instanceof Object[] array) {
                    jobs = add(jobs, new Job(type, Arrays.asList(array), out, at));
                } else if (type.kind() == Kind.LIST ? value instanceof List<?> : value instanceof Map<?, ?>) {
                    jobs = add(jobs, new Job(type, value, out, at));
                } else {
                    throw Slots.mismatch(type, value);
                }
            }
            default -> out[at] = Slots.scalar(type, value, owner, makers);
        }
        return jobs;
    }

    private List<Job> fill(List<Type.Field> fields, Object[] values, long[] out, int at, List<Job> jobs)
            throws Throwable {
        for (int i = 0; i < values.length; ++i) {
            Type.Field f = fields.get(i);
            if (f.boxed()) {
                jobs = add(jobs, new Job(f.box(), Collections.singletonList(values[i]), out, at + f.offset()));
            } else {
                jobs = inline(f.type(), values[i], out, at + f.offset(), jobs);
            }
        }
        return jobs;
    }

    private static List<Job> add(List<Job> jobs, Job job) {
        List<Job> out = jobs == null ? new ArrayList<>(2) : jobs;
        out.add(job);
        return out;
    }

    /** Builds each job's list or map and stores it in the job's slot. */
    private final class Jobs extends Walk.Node {
        private final List<Job> jobs;

        Jobs(List<Job> jobs) {
            this.jobs = jobs;
        }

        @Override
        int size() {
            return jobs.size();
        }

        @Override
        Object part(int i) throws Throwable {
            Job job = jobs.get(i);
            return job.type.kind() == Kind.LIST ? new ListMaker(job.type, (List<?>) job.value)
                    : new MapMaker(job.type, (Map<?, ?>) job.value);
        }

        @Override
        void accept(int i, Object value) {
            Job job = jobs.get(i);
            job.out[job.at] = (Long) value;
        }

        @Override
        Object finish() {
            return null;
        }
    }

    private final class ListMaker extends Walk.Node {
        private final Type type;
        private final Type element;
        private final int width;
        private final Iterator<?> elements;
        private final long[] slots;
        private final int count;
        private final int size;

        ListMaker(Type type, List<?> list) throws Throwable {
            this.type = type;
            this.element = type.element();
            this.width = element.slots();
            this.count = list.size();
            this.elements = list.iterator();
            this.slots = new long[Math.max(1, Math.multiplyExact(count, width))];
            boolean eager = !element.holdsContainers();
            if (eager && width == 1 && !Slots.isComposite(element.kind())) {
                for (int i = 0; i < count; ++i) {
                    slots[i] = Slots.scalar(element, elements.next(), owner, makers);
                }
            } else if (eager) {
                for (int i = 0; i < count; ++i) {
                    inline(element, elements.next(), slots, i * width, null);
                }
            }
            this.size = eager ? 0 : count;
        }

        @Override
        int size() {
            return size;
        }

        @Override
        Object part(int i) throws Throwable {
            List<Job> jobs = inline(element, elements.next(), slots, i * width, null);
            return jobs == null ? null : new Jobs(jobs);
        }

        @Override
        void accept(int i, Object value) {}

        @Override
        Object finish() throws Throwable {
            return makers.list(type, slots, count);
        }
    }

    private final class MapMaker extends Walk.Node {
        private final Type type;
        private final int keyWidth;
        private final int valueWidth;
        private final Iterator<? extends Map.Entry<?, ?>> entries;
        private final long[] keys;
        private final long[] values;
        private final int count;
        private final int size;

        MapMaker(Type type, Map<?, ?> map) throws Throwable {
            this.type = type;
            this.keyWidth = type.key().slots();
            this.valueWidth = type.element().slots();
            this.count = map.size();
            this.entries = map.entrySet().iterator();
            this.keys = new long[Math.max(1, Math.multiplyExact(count, keyWidth))];
            this.values = new long[Math.max(1, Math.multiplyExact(count, valueWidth))];
            boolean eager = !type.key().holdsContainers() && !type.element().holdsContainers();
            for (int i = 0; eager && i < count; ++i) {
                part(i);
            }
            this.size = eager ? 0 : count;
        }

        @Override
        int size() {
            return size;
        }

        @Override
        Object part(int i) throws Throwable {
            Map.Entry<?, ?> e = entries.next();
            List<Job> jobs = inline(type.key(), e.getKey(), keys, i * keyWidth, null);
            jobs = inline(type.element(), e.getValue(), values, i * valueWidth, jobs);
            return jobs == null ? null : new Jobs(jobs);
        }

        @Override
        void accept(int i, Object value) {}

        @Override
        Object finish() throws Throwable {
            return makers.map(type, keys, values, count);
        }
    }
}
