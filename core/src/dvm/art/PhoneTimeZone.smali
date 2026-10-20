# meglet.runtime.PhoneTimeZone: where libcore learns the phone's time zone (hk_dvm_art.c, "the time zone").
#
# java.util.TimeZone's default is asked of org.apache.harmony.luni.internal.util.TimezoneGetter first; libcore itself has no
# getter and says GMT without one. On Android the framework's start gives it one that reads the property
# persist.sys.timezone (RuntimeInit.commonInit). This is that getter for the runtime here: its one method is native and
# answers the zone the shell named, or null, which is libcore's GMT as before.
.class public final Lmeglet/runtime/PhoneTimeZone;
.super Lorg/apache/harmony/luni/internal/util/TimezoneGetter;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Lorg/apache/harmony/luni/internal/util/TimezoneGetter;-><init>()V
    return-void
.end method

.method public native getId()Ljava/lang/String;
.end method
