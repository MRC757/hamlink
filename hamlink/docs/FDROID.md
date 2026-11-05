# Publishing HamLink on F-Droid

F-Droid builds HamLink from this repository on its own servers and signs it with its own key. This page lists what is
already prepared, the steps to submit, and what each later release needs.

## What is prepared

| Item | Where |
|---|---|
| Build metadata for F-Droid | `hamlink/fdroid/org.hamlink.app.yml` (copied into F-Droid's metadata repository) |
| Store listing: title, short and full description | `fastlane/metadata/android/en-US/` (repository root) |
| Icon (512 x 512) | `fastlane/metadata/android/en-US/images/icon.png`, drawn by `hamlink/tools/make_icon.py`; the app's launcher icon is the same design (`res/drawable/ic_launcher_foreground.xml`) |
| Screenshots (GPS lines blurred) | `fastlane/metadata/android/en-US/images/phoneScreenshots/` |
| Changelog per version | `fastlane/metadata/android/en-US/changelogs/<versionCode>.txt` (7 = 0.2.3) |
| NDK version pinned | `ndkVersion = "27.0.12077973"` in `hamlink/android/app/build.gradle.kts` |
| Prebuilt binaries | Only `hamlink/windows/native/hamlink_core.dll`, which the Android build does not use; the metadata deletes it before building (`scandelete`). The MT63 `.dat` files are C source. |

Checked against the [Inclusion Policy](https://f-droid.org/docs/Inclusion_Policy/):
* **Licence:** GPL-3.0-or-later.
* **Code:** no proprietary libraries, ads or tracking. The native code (Codec2, the Rattlegram modem, MT63) is built
  from source.
* **Dependencies:** come from Google Maven, Maven Central and JitPack (`usb-serial-for-android`, MIT). F-Droid lists
  JitPack among its trusted Maven repositories.
* **Release signing:** without the project key (`%USERPROFILE%/.hamlink-release/`), release builds are unsigned, which
  is what F-Droid's build server wants.

## Steps

1. **Release 0.2.3** as usual. The release commit must contain everything above, with `versionCode 7` and
   `versionName "0.2.3"`. Then push a tag:
   ```
   git tag -a v0.2.3 -m "HamLink 0.2.3"
   git push origin v0.2.3
   ```
   F-Droid builds from the tag (`commit: v0.2.3`) and finds later versions from new `v*` tags.

2. **Test the build F-Droid's way** (optional but saves a review round). On Linux, or WSL on Windows, with Docker:
   ```
   git clone https://gitlab.com/fdroid/fdroiddata && cd fdroiddata
   cp /path/to/hamlink/fdroid/org.hamlink.app.yml metadata/
   docker run --rm -v "$PWD":/repo registry.gitlab.com/fdroid/fdroidserver:buildserver \
       build --verbose --test --latest org.hamlink.app
   docker run --rm -v "$PWD":/repo registry.gitlab.com/fdroid/fdroidserver:buildserver lint org.hamlink.app
   ```
   `build` must finish with an unsigned APK; `lint` must report nothing.

3. **Submit:**
   1. Make an account on gitlab.com.
   2. Fork https://gitlab.com/fdroid/fdroiddata.
   3. Create a branch, add `metadata/org.hamlink.app.yml`, commit, push, and open a merge request.
   4. Use the "App inclusion" template and tick its checklist.

   Reviewers often ask for small changes, and acceptance usually takes a few weeks. After merging, the app appears
   at the next index update (a day or two).

## Each later release

1. Raise `versionCode` and `versionName` in `hamlink/android/app/build.gradle.kts`.
2. Add `fastlane/metadata/android/en-US/changelogs/<versionCode>.txt` (500 characters at most).
3. Tag `vX.Y.Z` and push the tag.

F-Droid's update checker (`UpdateCheckMode: Tags`) finds the tag and builds the new version by itself; nothing needs
changing in fdroiddata.

## Signing: F-Droid copies and GitHub copies are separate

F-Droid signs with its own key. A phone with the GitHub APK (signed with the HamLink release key) cannot update to the
F-Droid build or back; switching means uninstalling, which loses settings and message history. Tell users to pick
one source.

To publish the project's own signed APK through F-Droid instead, the build has to be **reproducible**: F-Droid
rebuilds it, checks that it matches the GitHub APK byte for byte, then ships the GitHub one. That is a later
improvement. Native code needs care to build reproducibly; see
https://f-droid.org/docs/Reproducible_Builds/.
