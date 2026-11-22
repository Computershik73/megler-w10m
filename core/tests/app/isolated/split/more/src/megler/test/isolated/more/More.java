package megler.test.isolated.more;

import megler.test.isolated.browser.Browser;

/** Code of the split "more", which uses the split "browser": its class loader's parent is that split's, whose classes it sees. */
public final class More {
    private More() {}

    public static String hello() {
        return "more over " + Browser.name() + " " + (Browser.class.getClassLoader() == More.class.getClassLoader().getParent());
    }
}
