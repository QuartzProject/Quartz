package org.quartzlang.jvm;

import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Finds libquartz: the {@code quartz.library} property, then {@code java.library.path}, then the jar's natives. */
final class NativeLoader {
    private NativeLoader() {}

    static SymbolLookup load() {
        String explicit = System.getProperty("quartz.library");
        if (explicit != null && !explicit.isEmpty()) {
            return SymbolLookup.libraryLookup(Path.of(explicit), Arena.global());
        }
        for (String name : fileNames()) {
            for (String dir : System.getProperty("java.library.path", "").split(java.io.File.pathSeparator)) {
                if (dir.isEmpty()) {
                    continue;
                }
                Path candidate = Path.of(dir, name);
                if (Files.isRegularFile(candidate)) {
                    return SymbolLookup.libraryLookup(candidate, Arena.global());
                }
            }
        }
        return SymbolLookup.libraryLookup(extract(), Arena.global());
    }

    static String platform() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String arch = switch (System.getProperty("os.arch", "").toLowerCase(Locale.ROOT)) {
            case "amd64", "x86_64" -> "x86_64";
            case "aarch64", "arm64" -> "aarch64";
            default -> System.getProperty("os.arch", "unknown");
        };
        String family = os.startsWith("windows") ? "windows" : os.startsWith("mac") ? "macos" : "linux";
        return family + "-" + arch;
    }

    private static List<String> fileNames() {
        List<String> names = new ArrayList<>(List.of(System.mapLibraryName("quartz")));
        if (platform().startsWith("windows")) {
            names.add("libquartz.dll");
        }
        return names;
    }

    private static Path extract() {
        String platform = platform();
        for (String name : fileNames()) {
            String resource = "/natives/" + platform + "/" + name;
            try (InputStream in = NativeLoader.class.getResourceAsStream(resource)) {
                if (in == null) {
                    continue;
                }
                Path dir = Files.createTempDirectory("quartz-jvm");
                Path file = dir.resolve(name);
                Files.copy(in, file, StandardCopyOption.REPLACE_EXISTING);
                file.toFile().deleteOnExit();
                dir.toFile().deleteOnExit();
                return file;
            } catch (IOException e) {
                throw new UncheckedIOException("cannot extract " + resource, e);
            }
        }
        throw new UnsatisfiedLinkError("no libquartz for " + platform + ": set -Dquartz.library=<file>, add it to "
                + "java.library.path or use a jar that carries natives/" + platform + "/");
    }
}
