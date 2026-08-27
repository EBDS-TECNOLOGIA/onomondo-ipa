# Installing as a privileged system app

The IPA needs two things the Android framework only gives to a system app:
`MODIFY_PHONE_STATE`, and the LPA identity that guards ISD-R access (see
[../README.md](../README.md), "Privileges — two gates, not one"). A side-load
gets neither, no matter how many permissions you grant by hand — "debuggable"
is not "privileged".

Re-run this procedure per device.

## Before you start: is this device even eligible?

`adb root` must work. On a production **`user`** build it does not, and neither
`disable-verity` nor `remount` is available — there is no way to write to
`/system`, so this whole path is closed. Your options there are a vendor-signed
system image or having the integrator ship the APK in the image.

    adb shell getprop ro.build.type      # userdebug or eng => you can proceed
    adb root                             # must print "restarting adbd as root"

Everything below assumes `userdebug`/`eng`.

## 1. Unlock the bootloader — **this factory-resets the device**

Back up anything you care about first; unlocking wipes userdata by design.

    # On the device: Settings -> About -> tap Build number x7,
    # then Developer options -> enable "OEM unlocking" AND "USB debugging".

    adb reboot bootloader
    fastboot flashing unlock        # older devices: fastboot oem unlock
    # Confirm on the device screen (volume keys to select, power to accept).
    fastboot reboot

The device comes back factory-fresh: walk the setup wizard again and re-enable
**USB debugging** (and, if you will re-flash later, "OEM unlocking").

## 2. Turn off dm-verity and make /system writable

    adb root
    adb disable-verity
    adb reboot

    adb root
    adb remount                     # mounts /system rw (overlayfs on modern devices)

`disable-verity` only takes effect after the reboot, which is why `remount`
comes after it and not before. `adb remount` sometimes needs a second run right
after boot; if it reports success but writes still fail, reboot once more.

Verity stays off across reboots, but a factory reset or an OTA can turn it back
on — then repeat this step.

## 3. Push the APK

Pick a directory name for the app under `/system/priv-app/`. **The name is
arbitrary** — PackageManager scans `/system/priv-app/*/` for APKs and does not
care what the directory or the APK file is called. Nothing in this repo
references it. What must match is the `package=` in the allowlist below and the
module's `applicationId`.

**Exactly one APK per directory.** A directory under `/system/priv-app/` is
parsed as a single package "cluster": one base APK, plus splits. A second base
APK makes the *whole directory* fail to parse, and PackageManager then drops
every app in it -- including one that was working before you added the second
file. The failure is silent from the UI: no app, no error, nothing in Settings.
So when you replace an APK whose filename has changed, **delete the old file**,
do not just push alongside it.

Use one directory per app, and pick something unlikely to collide with a
vendor app already in that directory:

    PRIVAPP_DIR=OnomondoIpa          # <- your choice; used only on the device

    adb root && adb remount
    adb shell mkdir -p /system/priv-app/$PRIVAPP_DIR
    adb push app-generic/build/outputs/apk/debug/app-generic-debug.apk \
        /system/priv-app/$PRIVAPP_DIR/

    adb shell chmod 755 /system/priv-app/$PRIVAPP_DIR
    adb shell chmod 644 /system/priv-app/$PRIVAPP_DIR/app-generic-debug.apk
    adb shell restorecon -R /system/priv-app/$PRIVAPP_DIR

`restorecon` matters: a file written over `adb push` can land with a context
that PackageManager will not read, and the failure looks like the app simply
not existing after reboot.

Install `app-generic`.

> **An application module carrying a vendor SDK will not boot with this
> allowlist.** The XML names `app-generic`'s applicationId and nothing else, and
> a vendor AAR readily merges dozens of extra permissions, many of them
> `signature|privileged` (`INSTALL_PACKAGES`, `DELETE_PACKAGES`,
> `NETWORK_SETTINGS`, `NETWORK_STACK`, `MANAGE_PROFILE_AND_DEVICE_OWNERS`,
> `WRITE_SECURE_SETTINGS`, `REBOOT`, `RECOVERY`, `INTERACT_ACROSS_USERS_FULL`,
> …). Any one of them missing from the allowlist stops the boot. Recover by
> deleting the directory (see "Recovering from a boot loop" below).
>
> `app-generic` exists precisely to avoid this: it needs three entries, and it
> reaches the eUICC through stock AOSP telephony anyway -- that is how the
> Phase-1 spike passed. If a device ever does need its own module, build its
> allowlist empirically (below) rather than by hand.

### Building an allowlist for an app with many privileged permissions

Guessing the list is how you get a second boot loop; let the platform tell you.
Set the policy to log-only, install, boot, and read off exactly what it wanted:

    adb shell setprop ro.control_privapp_permissions log   # or edit /system/build.prop
    # install the priv-app as above, reboot, then:
    adb logcat -b all -d | grep -i 'privapp-permissions'

Each line names one package and one permission. Turn those into
`<permission name="..."/>` entries, push the XML, set the property back to
`enforce`, and reboot. Verify with the `dumpsys` check in step 5.

### Recovering from a boot loop

An unallowlisted priv-app leaves the device cycling on the boot animation.
`adbd` starts early in init, so the fix is usually still reachable over adb even
though the boot never completes:

    adb wait-for-device
    adb root && adb remount
    adb shell rm -rf /system/priv-app/<TheDirectoryYouAdded>
    adb reboot

If adb never comes up, boot to the bootloader and use recovery to mount
`/system` and delete the same directory. Nothing is corrupted -- one file is
refusing the boot, and removing it is the whole repair.

## 4. Push the permission allowlist

    adb push privapp-permissions-ipaspike.xml /system/etc/permissions/
    adb shell chmod 644 /system/etc/permissions/privapp-permissions-ipaspike.xml
    adb shell restorecon /system/etc/permissions/privapp-permissions-ipaspike.xml

It must live on the **same partition** as the priv-app: `/system/etc/permissions/`
for `/system/priv-app`. If you put the APK under `/product/priv-app` or
`/system_ext/priv-app`, the XML goes in that partition's `etc/permissions/`.

> **Do not skip this, and do not let it drift from the APK.** Since Android 9, a
> priv-app holding a `signature|privileged` permission that is not in an
> allowlist makes the device **fail to boot** (logcat: "Signature|privileged
> permissions not in privapp-permissions allowlist"). If you ever change
> `app-generic`'s `applicationId`, push the new APK and the updated XML in the
> same session — a mismatch is a fastboot/recovery recovery, not a reboot.

## 5. Reboot and verify

    adb reboot
    adb shell dumpsys package com.onomondo.ipa.spike.generic | grep -A6 'requested permissions'

`MODIFY_PHONE_STATE`, `READ_PRIVILEGED_PHONE_STATE` and
`WRITE_EMBEDDED_SUBSCRIPTIONS` should all show as granted. If they show as
denied, the allowlist did not take (wrong partition, wrong context, or the
package name does not match).

Then launch **IPAd** and grant the notification permission when it asks, or the
foreground service starts without its notification.

## Upgrading later

Same package name = a normal in-place replacement: push the new APK over the
old one and reboot. Do not re-push the XML unless the permissions or the
`applicationId` changed.

**Check what is already in the directory first**, and leave exactly one APK
behind:

    adb shell ls -l /system/priv-app/$PRIVAPP_DIR/

If the new APK has a different filename than the installed one -- or you are
moving to a differently-named directory -- remove the old file (or the old
directory entirely, `rm -rf`, which also clears its stale `oat/` dexopt
artifacts). Two base APKs in one directory is the single easiest way to make a
previously working priv-app vanish; see the warning in step 3.

Also skip steps 1 and 2 on a device that is already prepared. Re-running
`fastboot flashing unlock` on an unlocked device still factory-resets it, and a
factory reset re-enables verity -- undoing exactly what you are trying to keep.

The APK is debug-signed. If `~/.android/debug.keystore` has been regenerated
since the first install, the upgrade is rejected on signature mismatch —
`adb uninstall com.onomondo.ipa.spike.generic` clears the data-partition copy
first.
