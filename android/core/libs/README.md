# euicc-system-stubs.jar

`compileOnly` stubs for the `android.service.euicc.*` **@SystemApi** classes
(EuiccService + its result/callback types), which are NOT in the public SDK
android.jar. They exist only so `IpaEuiccService` can subclass `EuiccService`
at compile time; the real classes are provided by the framework at runtime, so
this jar is never packaged into the APK.

Origin: `android/service/euicc/*.class` extracted from a system-API android.jar
for API 34 (compileSdk 34). Regenerate for a new API level by extracting the
same package from that level's system android.jar.
