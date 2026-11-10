package megler.test.scroll;

import android.app.Activity;
import android.graphics.Outline;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.view.ViewTreeObserver;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;

/**
 * A screen made as WhatsApp's first one is (com.whatsapp.registration.app.EULA, 2.26): a ScrollView over a column taller than
 * it, whose bottom row -- just below the ScrollView's own bottom edge, where the screen has to be scrolled to see it -- holds
 * two cards, each a frame clipped to its outline, raised, with a button in it, as androidx's CardView is (CardViewApi21Impl:
 * a rounded background, setClipToOutline, setElevation). Card A keeps the background it was made with, and has an outline
 * of its own 2 px inside it. Card B is given a plain colour once the screen is laid out, before its first frame, as WhatsApp
 * does from the ScrollView's global layout listener when its content is taller than it: CardView.setCardElevation(8),
 * View.setBackgroundColor -- which puts a new ColorDrawable there, its bounds empty until the card is drawn.
 *
 * F12 scrolls the screen to the bottom, and the app says where the cards are then. Pixels in pixels: the test runs at 160 dpi.
 */
public class Main extends Activity {
    static final int PAGE = 0xFF0A1014, TOP = 0xFF2060C0, CARD_A = 0xFF604080, CARD_B_FIRST = 0xFF808080, CARD_B = 0xFF406080, BUTTON = 0xFF21C063,
        CORNER = 0xFFC02020;
    static final int VIEWPORT = 400, CARD = 56, BOTTOM_PADDING = 24;
    private ScrollView scroll;
    private LinearLayout column;
    private FrameLayout cardA, cardB;
    private boolean shown;

    private static void say(String what) {
        Log.i("ScrollTest", what);
    }

    private static String place(View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight();
    }

    /**
     * A card as CardView makes one on Android 5 and later: a rounded background, clipped to its outline, raised; a button in it.
     * Under the button, a square of CORNER in its top left corner, which the outline cuts where it does not reach the corner.
     */
    private FrameLayout card(int colour) {
        FrameLayout card = new FrameLayout(this);
        GradientDrawable round = new GradientDrawable();
        round.setColor(colour);
        round.setCornerRadius(6);
        card.setBackground(round);
        card.setClipToOutline(true);
        card.setElevation(4);
        View corner = new View(this);
        corner.setBackgroundColor(CORNER);
        card.addView(corner, new FrameLayout.LayoutParams(20, 20));
        View button = new View(this);
        button.setBackgroundColor(BUTTON);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        lp.setMargins(12, 8, 12, 8);
        card.addView(button, lp);
        return card;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(PAGE);
        scroll = new ScrollView(this);
        column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(0, 0, 0, BOTTOM_PADDING);
        FrameLayout top = new FrameLayout(this);
        top.setBackgroundColor(TOP);
        column.addView(top, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, VIEWPORT));
        LinearLayout row = new LinearLayout(this);
        cardA = card(CARD_A);
        cardB = card(CARD_B_FIRST);
        // Card A's outline 2 px inside its bounds, which the test sees cut its corner square: the outline is clipped to.
        cardA.setOutlineProvider(new ViewOutlineProvider() {
            @Override
            public void getOutline(View v, Outline o) {
                o.setRect(2, 2, v.getWidth() - 2, v.getHeight() - 2);
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, CARD, 1);
        lp.setMargins(8, 0, 8, 0);
        row.addView(cardA, lp);
        lp = new LinearLayout.LayoutParams(0, CARD, 1);
        lp.setMargins(8, 0, 8, 0);
        row.addView(cardB, lp);
        column.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        scroll.addView(column);
        root.addView(scroll, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, VIEWPORT));
        setContentView(root);
        // WhatsApp's way: once laid out, the content found taller than its ScrollView, the card at the bottom raised and given a
        // plain colour -- after the layout, before the first frame.
        scroll.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                scroll.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                int over = column.getHeight() - scroll.getMeasuredHeight();
                if (over > 0) {
                    cardB.setElevation(8);
                    cardB.setBackgroundColor(CARD_B);
                }
                say("laid out over=" + over + " cardB=" + cardB.getBackground().getClass().getSimpleName() + " bounds=" + cardB.getBackground().getBounds().toShortString());
            }
        });
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus || shown) return;
        shown = true;
        say("shown scroll=" + place(scroll) + " a=" + place(cardA) + " b=" + place(cardB));
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_F12) {
            // To the bottom, as WhatsApp's screen went by itself on the phone (and by a finger's drag): the cards come into view.
            final int range = column.getHeight() - scroll.getHeight();
            scroll.scrollTo(0, range);
            scroll.post(() -> say("scrolled y=" + scroll.getScrollY() + " range=" + range + " a=" + place(cardA) + " b=" + place(cardB)));
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("destroyed");
    }
}
