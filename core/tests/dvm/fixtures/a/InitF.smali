# A class without a constructor of its own, as R8 leaves one whose constructor only passed its arguments on to its
# superclass's: its callers call InitF's <init> for InitA's.
.class public Lt/InitF;
.super Lt/InitA;
