## Vermeer native Linux driver

This development fork starts from `430e24e` of the Goodix QSEE reference fork,
which supplies misc-device discovery to libfprint. The new `goodixvermeer`
driver has a separate protocol implementation: it uses the native Linux QTEE
backend in `../vermeer-fingerprint`. It does not use the reference sensor's
command layout, Android HAL libraries or hardware-auth-token provider.

The driver binds `goodix_fp` with DT compatible `xiaomi,vermeer-fingerprint`.
Matching stays inside the signed Vermeer TA. libfprint prints contain a Linux
group/finger reference, and identification is checked against the provided
libfprint gallery. Enrollment is authorized by normal fprintd policy and the
root-only native backend.

The backend and KDE target are maintained in
[vermeer-fingerprint](https://github.com/zuwwik/vermeer-fingerprint), with the
filesystem listener in
[vermeer-qtee-supplicant](https://github.com/zuwwik/vermeer-qtee-supplicant).
Kernel changes are on
[vermeer-mainline's fingerprint-vermeer branch](https://github.com/zuwwik/vermeer-mainline/tree/fingerprint-vermeer).

Build the backend first and expose its pkg-config file, then configure:

```sh
PKG_CONFIG_PATH=/path/to/native/lib/pkgconfig meson setup build \
  -Ddrivers=goodixvermeer -Ddoc=false -Dintrospection=false
meson compile -C build
meson test -C build --print-errorlogs
```

A native session component must render the optical target and report stable
contact through the illumination interface. See the backend repository's
README and packaging directory for the KDE component and filesystem listener.
Firmware and per-phone factory calibration are private runtime inputs; they
are not included here.

Live fprintd pilot tests have passed enrollment, matching, rejection and
cancellation. The Nura target build passes six enabled tests, including the
illumination interface tests. Tests for unrelated disabled drivers are skipped.
The native KDE frontend is installed on the test phone. With the corrected
backend reset/wakeup sequence, three consecutive lock-screen sessions matched
on their first capture; an unregistered finger was rejected. Long-term
reliability and false-accept/false-reject rates remain unmeasured.



<div align="center">

# LibFPrint

*LibFPrint is part of the **[FPrint][Website]** project.*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

</div>

## History

**LibFPrint** was originally developed as part of an
academic project at the **[University Of Manchester]**.

It aimed to hide the differences between consumer
fingerprint scanners and provide a single uniform
API to application developers.

## Goal

The ultimate goal of the **FPrint** project is to make
fingerprint scanners widely and easily usable under
common Linux environments.

## License

`Section 6` of the license states that for compiled works that use
this library, such works must include **LibFPrint** copyright notices
alongside the copyright notices for the other parts of the work.

**LibFPrint** includes code from **NIST's** **[NBIS]** software distribution.

We include **Bozorth3** from the **[US Export Controlled]**
distribution, which we have determined to be fine
being shipped in an open source project.

## Get in *touch*

 - [IRC] - `#fprint` @ `irc.oftc.net`
 - [Matrix] - `#fprint:matrix.org` bridged to the IRC channel
 - [MailingList] - low traffic, not much used these days

<br/>

<div align="right">

[![Badge License]][License]

</div>


<!----------------------------------------------------------------------------->

[Documentation]: https://fprint.freedesktop.org/libfprint-dev/
[Contributors]: https://gitlab.freedesktop.org/libfprint/libfprint/-/graphs/master
[Unsupported]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[Supported]: https://fprint.freedesktop.org/supported-devices.html
[Website]: https://fprint.freedesktop.org/
[MailingList]: https://lists.freedesktop.org/mailman/listinfo/fprint
[IRC]: ircs://irc.oftc.net:6697/#fprint
[Matrix]: https://matrix.to/#/#fprint:matrix.org

[Contribute]: ./HACKING.md
[License]: ./COPYING

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html


<!---------------------------------[ Badges ]---------------------------------->

[Badge License]: https://img.shields.io/badge/License-LGPL2.1-015d93.svg?style=for-the-badge&labelColor=blue


<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg
