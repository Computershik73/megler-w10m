# java.lang.String. Its text is kept by the runtime (UTF-16 after the object's header), so it has no fields
# and nearly everything is native.
.class public final Ljava/lang/String;
.super Ljava/lang/Object;
.implements Ljava/io/Serializable;
.implements Ljava/lang/Comparable;
.implements Ljava/lang/CharSequence;

# The constructors are declared so that code naming them links. They are never called: the interpreter
# makes the string itself where one is invoked (mg_dvm_string_init), as ART does through StringFactory.
.method public constructor native <init>()V
.end method

.method public constructor native <init>(Ljava/lang/String;)V
.end method

.method public constructor native <init>([C)V
.end method

.method public constructor native <init>([CII)V
.end method

.method public constructor native <init>([B)V
.end method

.method public constructor native <init>([BII)V
.end method

.method public constructor native <init>([BLjava/lang/String;)V
.end method

.method public constructor native <init>([BIILjava/lang/String;)V
.end method

.method public constructor native <init>(Ljava/lang/StringBuilder;)V
.end method

.method public native length()I
.end method

.method public native isEmpty()Z
.end method

.method public native charAt(I)C
.end method

.method public native equals(Ljava/lang/Object;)Z
.end method

.method public native hashCode()I
.end method

.method public toString()Ljava/lang/String;
    .registers 1
    return-object p0
.end method

.method public native concat(Ljava/lang/String;)Ljava/lang/String;
.end method

.method public native substring(I)Ljava/lang/String;
.end method

.method public native substring(II)Ljava/lang/String;
.end method

.method public subSequence(II)Ljava/lang/CharSequence;
    .registers 4
    invoke-virtual {p0, p1, p2}, Ljava/lang/String;->substring(II)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public native indexOf(I)I
.end method

.method public native indexOf(II)I
.end method

.method public native indexOf(Ljava/lang/String;)I
.end method

.method public native lastIndexOf(I)I
.end method

.method public native contains(Ljava/lang/CharSequence;)Z
.end method

.method public native startsWith(Ljava/lang/String;)Z
.end method

.method public native endsWith(Ljava/lang/String;)Z
.end method

.method public native compareTo(Ljava/lang/String;)I
.end method

# The bridge javac writes for Comparable.compareTo(Object).
.method public bridge synthetic compareTo(Ljava/lang/Object;)I
    .registers 3
    check-cast p1, Ljava/lang/String;
    invoke-virtual {p0, p1}, Ljava/lang/String;->compareTo(Ljava/lang/String;)I
    move-result v0
    return v0
.end method

.method public native equalsIgnoreCase(Ljava/lang/String;)Z
.end method

.method public native toLowerCase()Ljava/lang/String;
.end method

.method public native toUpperCase()Ljava/lang/String;
.end method

.method public native trim()Ljava/lang/String;
.end method

.method public native replace(CC)Ljava/lang/String;
.end method

.method public native toCharArray()[C
.end method

.method public native getBytes()[B
.end method

.method public native intern()Ljava/lang/String;
.end method

.method public static valueOf(Ljava/lang/Object;)Ljava/lang/String;
    .registers 2
    if-nez p0, :some
    const-string v0, "null"
    return-object v0
    :some
    invoke-virtual {p0}, Ljava/lang/Object;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method public static native valueOf(I)Ljava/lang/String;
.end method

.method public static native valueOf(J)Ljava/lang/String;
.end method

.method public static native valueOf(C)Ljava/lang/String;
.end method

.method public static native valueOf(Z)Ljava/lang/String;
.end method

.method public static native valueOf(F)Ljava/lang/String;
.end method

.method public static native valueOf(D)Ljava/lang/String;
.end method

.method public static native valueOf([C)Ljava/lang/String;
.end method
