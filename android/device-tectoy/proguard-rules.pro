# Keep the native-called transport + spike classes (referenced by JNI name).
-keep class com.onomondo.ipa.EuiccChannel { *; }
-keep class com.onomondo.ipa.spike.EuiccSpike { *; }

# Keep the Tectoy SDK surface we call reflectively/async.
-keep class br.com.tectoy.** { *; }
-keep class br.com.tectoylib.** { *; }
-dontwarn br.com.tectoy.**
-dontwarn br.com.tectoylib.**
