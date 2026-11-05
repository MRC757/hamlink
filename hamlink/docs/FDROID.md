# Publishing HamLink on F-Droid

F-Droid builds HamLink from this repository on its own servers and signs it with its own key. This page lists what is
already prepared, the steps to submit, and what each later release needs.

## What is prepared

| Item | Where |
|---|---|
| Build metadata for F-Droid | `hamlink/fdroid/org.hamlink.app.yml`, copied unchanged to `metadata/org.hamlink.app.yml` in F-Droid's metadata repository. It is in the canonical form `fdroid rewritemeta` produces, so it has no comments: it builds 0.2.4 from its tag's full commit hash in `hamlink/android/app`, pins the NDK, accepts only APKs signed with the release key (`AllowedAPKSigningKeys`, with the GitHub APK as `Binaries`). Categories: Connectivity and Messaging, from F-Droid's current list (`config/categories.yml` in fdroiddata; "Radio" there means broadcast-radio streaming apps). |
| Store listing: title, short and full description | `fastlane/metadata/android/en-US/` (repository root) |
| Icon (512 x 512) | `fastlane/metadata/android/en-US/images/icon.png`, drawn by `hamlink/tools/make_icon.py`; the app's launcher icon is the same design (`res/drawable/ic_launcher_foreground.xml`) |
| Screenshots (GPS lines blurred) | `fastlane/metadata/android/en-US/images/phoneScreenshots/` |
| Changelog per version | `fastlane/metadata/android/en-US/changelogs/<versionCode>.txt` (7 = 0.2.3, 8 = 0.2.4) |
| NDK version pinned | `ndkVersion = "27.0.12077973"` in `hamlink/android/app/build.gradle.kts` |
| Prebuilt binaries | Only `hamlink/windows/native/hamlink_core.dll`, which the Android build does not use. F-Droid's scanner does not flag it, so it needs no `scandelete` (an unused `scandelete` path fails the build). The MT63 `.dat` files are C source. |

Checked against the [Inclusion Policy](https://f-droid.org/docs/Inclusion_Policy/):
* **Licence:** GPL-3.0-or-later.
* **Code:** no proprietary libraries, ads or tracking. The native code (Codec2, the Rattlegram modem, MT63) is built
  from source.
* **Dependencies:** come from Google Maven, Maven Central and JitPack (`usb-serial-for-android`, MIT). F-Droid lists
  JitPack among its trusted Maven repositories.
* **Release signing:** without the project key (`%USERPROFILE%/.hamlink-release/`), release builds are unsigned, which
  is what F-Droid's build server wants.

## Reproducible builds

F-Droid publishes **the project's own signed APK**: it rebuilds HamLink from the release tag, copies the signature
from the APK on GitHub onto its build, and publishes the result if it verifies. So the GitHub APK must be exactly what
anyone gets by building that tag. Users can then move between the GitHub and F-Droid copies.

What makes the build reproducible:
* **Native code** (`android/app/src/main/cpp/CMakeLists.txt`): build paths mapped to fixed names
  (`-ffile-prefix-map`), and no build-id note.
* **Gradle** (`android/app/build.gradle.kts`): no dependency-info block, no version-control info file, no baseline
  profiles.
* **Release APK built in CI**: `.github/workflows/android-release.yml` builds the unsigned APK twice, from checkouts in
  two different directories, and fails unless they are byte-identical. It runs on every `v*` tag.
* **Signing** (`tools/sign_release_apk.sh`, on the maintainer's PC; the key never goes to GitHub): v2 + v3 only, with
  the zip alignment preserved, so the signed APK is the unsigned one plus apksigner's padding and signing block.

## Each release

1. **Bump the version.** Raise `versionCode` and `versionName` in `android/app/build.gradle.kts`, and `<Version>` in
   `windows/HamLink.csproj`.
2. **Changelog.** Add `fastlane/metadata/android/en-US/changelogs/<versionCode>.txt` (500 characters at most).
3. **Tag.** Commit, then tag and push it: `git tag -a vX.Y.Z -m "HamLink X.Y.Z" && git push origin vX.Y.Z`.
4. **Sign the CI build.** Wait for the `android-release` run on the tag to pass (the reproducibility check), then
   download and sign its APK:
   ```
   gh run download <run-id> -n hamlink-android-unsigned
   hamlink/tools/sign_release_apk.sh app-release-unsigned.apk HamLink-X.Y.Z-android.apk
   ```
5. **Publish.** Create the GitHub release with that APK and the Windows zip. The APK must be named
   `HamLink-X.Y.Z-android.apk`: F-Droid's metadata downloads it from
   `https://github.com/MRC757/hamlink/releases/download/vX.Y.Z/HamLink-X.Y.Z-android.apk`.

**F-Droid picks up new tags by itself**, so nothing needs changing in fdroiddata. Do not attach an APK built any other
way (for example on Windows): F-Droid's build would not match it, and that version would not be published.

## First submission

1. **Fork fdroiddata.** In https://gitlab.com/fdroid/fdroiddata, fork with your own namespace, **Public**, default
   branch only (done: gitlab.com/mrc757/fdroiddata).
2. **Add the metadata.** Put `hamlink/fdroid/org.hamlink.app.yml` at `metadata/org.hamlink.app.yml` on a new branch
   (`org.hamlink.app`). The `commit` field must be the **full hash** of the release tag's commit. (Done for 0.2.4.)
3. **Check it.** Run `fdroid lint org.hamlink.app` and `fdroid rewritemeta org.hamlink.app` in the fdroiddata
   checkout: lint reports nothing, and rewritemeta changes nothing (on Windows it rewrites the line endings to CRLF;
   keep the LF file). (Done.)
4. **Open the merge request.** Title it "New app: HamLink" and use the default (app inclusion) template. Reviewers
   often ask for small changes, and acceptance usually takes a few weeks. After merging, the app appears at the next
   index update (a day or two).

## Submission status
* **2026-11-05:** merge request [!55912](https://gitlab.com/fdroid/fdroiddata/-/merge_requests/55912) opened. Every
  pipeline check passed, including the reproducible-build comparison against the signed GitHub APK. A maintainer closed
  it under Inclusion Policy *Quality point 6*: an app must have been developed in public for **at least 3 months**
  (this repository starts on 2026-11-01), unless it has proven users or the author maintains other F-Droid apps.
* **Resubmit on or after 2027-02-01:** update the metadata to the latest release on branch `org.hamlink.app` of
  gitlab.com/mrc757/fdroiddata, then reopen !55912 (or open a new merge request) with a short note on the development
  and radio testing since November.

## Signing history
0.2.2 and earlier: development (debug) key. 0.2.3: HamLink release key (APK built on Windows, not reproducible).
0.2.4 onward: HamLink release key, reproducible. Release key certificate SHA-256:
`58435591f0999a47f867baf7c176246248d42ea298c7770264eb7be42534586d`.
