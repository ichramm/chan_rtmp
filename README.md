chan_rtmp
=========

---
A RTMP Channel for Asterisk

* Writen in C using [librtmp].
* Asterisk **1.4** only, trying to upgrade to Asterisk 11.
* This modules supports **realtime only**, support for static peers is planed but had no time.


Installation
------------

```sh
export ASTERISK_PREFIX=/usr
[[ "$ASTERISK_PREFIX" = "/usr" ]] && export ASTERISK_ETC_DIR="" || export ASTERISK_ETC_DIR="$ASTERISK_PREFIX/etc"

git clone [git-repo-url] chan_rtmp
cd chan_rtmp
make
cp bin/chan_rtmp.so $ASTERISK_PREFIX/lib/asterisk/modules
cp conf/rtmp.conf $ASTERISK_ETC_DIR/
```

Client
------

The client uses a modified versions of [rtmplite], which may or may not work in a browser since it was designer to work with the adobe air runtime.


[librtmp]:http://rtmpdump.mplayerhq.hu/
[rtmplite]:https://code.google.com/p/rtmplite/
