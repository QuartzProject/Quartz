-keepclasseswithmembernames class org.quartzlang.kmp.QuartzJni {
    native <methods>;
}
-keep class org.quartzlang.kmp.CBinding {
    boolean dispatch(long, long, long);
}
