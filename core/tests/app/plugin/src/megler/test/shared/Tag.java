package megler.test.shared;

import java.io.Serializable;

/** A Serializable both test apps have, each its own copy of this file: read in the other app as that app's class. */
public final class Tag implements Serializable {
    private static final long serialVersionUID = 1L;
    public final String name;
    public final int weight;

    public Tag(String name, int weight) {
        this.name = name;
        this.weight = weight;
    }

    @Override
    public String toString() { return "Tag(" + name + ", " + weight + ")"; }
}
