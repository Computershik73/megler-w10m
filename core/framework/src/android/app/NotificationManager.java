package android.app;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Objects;

/*
 * Megler: an app's notification channels and groups are kept as Android's system keeps them for the app
 * (NotificationManagerService and its PreferencesHelper), which outlives the app's process:
 *
 * - One set for the process, whichever NotificationManager is asked (each kept its own in maps of its own, though
 *   ContextImpl hands every context the one it made), and kept in the app's data folder (FILE: Android's elements of its
 *   notification policy file, NotificationChannel.writeXml and NotificationChannelGroup.writeXml), read at the first call
 *   and written again on a thread of their own after each change. They were gone at each start of the app: Messenger,
 *   which looks its channels up by id, found "messenger_orca_800_live_location" missing at every start ("will reinitialize
 *   channels") and made them all again, on a Lumia 650 (0.1.0.685). Clearing the app's data clears them, as Android's.
 * - A channel is the system's copy: what the app made is copied (through a Parcel, as Android's binder hands it over), and
 *   each one handed out is a copy too; a change to either is the app's own until it makes the channel again.
 * - Making a channel that is there already changes only what Android lets an app change: its name, its description, its
 *   group where it had none, whether the user may block it, and its importance downwards (the user changed none of it here).
 *   A channel made anew has the system's own fields as an app's new channel has them on Android: no Do Not Disturb, no lock
 *   screen visibility of its own, bubbles as the user has them, not an important conversation, nothing locked.
 * - Deleting a channel marks it deleted (gone from the app's view), and making it again brings it back as it was; a deleted
 *   channel is forgotten 30 days later, as Android's. Deleting a group deletes its channels.
 * - getNotificationChannel(id, conversationId) (Android 11) is the conversation's channel (setConversationId) where there is
 *   one, else the channel itself; a group is handed out with its channels in it.
 * No exception is thrown where Android's system refuses a channel (an empty name, a group that is not there, the default
 * channel's id, a conversation without its parent): an app made for Android meets none of those.
 */
public class NotificationManager {
    public static final int IMPORTANCE_UNSPECIFIED = -1000, IMPORTANCE_NONE = 0, IMPORTANCE_MIN = 1, IMPORTANCE_LOW = 2, IMPORTANCE_DEFAULT = 3, IMPORTANCE_HIGH = 4, IMPORTANCE_MAX = 5,
        INTERRUPTION_FILTER_ALL = 1, INTERRUPTION_FILTER_PRIORITY = 2, INTERRUPTION_FILTER_NONE = 3, INTERRUPTION_FILTER_ALARMS = 4, INTERRUPTION_FILTER_UNKNOWN = 0;
    public static final String ACTION_NOTIFICATION_POLICY_ACCESS_GRANTED_CHANGED = "android.app.action.NOTIFICATION_POLICY_ACCESS_GRANTED_CHANGED", ACTION_APP_BLOCK_STATE_CHANGED = "android.app.action.APP_BLOCK_STATE_CHANGED";
    public void notify(int id, Notification n) { notify(null, id, n); }
    public void notify(String tag, int id, Notification n) { android.util.Log.i("Megler", "notification " + id + ": " + (n != null ? n.meglerText() : "")); }
    public void cancel(int id) {} public void cancel(String tag, int id) {} public void cancelAll() {}

    public void createNotificationChannel(NotificationChannel channel) { createNotificationChannels(java.util.Arrays.asList(channel)); }
    public void createNotificationChannels(List<NotificationChannel> channels) {
        synchronized (sLock) {
            load();
            boolean changed = false;
            for (NotificationChannel c : channels) changed |= create(c);
            if (changed) save();
        }
    }
    public void createNotificationChannelGroup(NotificationChannelGroup group) { createNotificationChannelGroups(java.util.Arrays.asList(group)); }
    public void createNotificationChannelGroups(List<NotificationChannelGroup> groups) {
        synchronized (sLock) {
            load();
            boolean changed = false;
            for (NotificationChannelGroup g : groups) changed |= create(g);
            if (changed) save();
        }
    }
    public NotificationChannel getNotificationChannel(String channelId) { return getNotificationChannel(channelId, null); }
    /** The conversation's channel (Android 11) where the app made one for it, else the channel itself (Android's). */
    public NotificationChannel getNotificationChannel(String channelId, String conversationId) {
        if (channelId == null) channelId = NotificationChannel.DEFAULT_CHANNEL_ID;
        synchronized (sLock) {
            load();
            if (conversationId != null) {
                for (NotificationChannel c : sChannels.values())
                    if (!c.isDeleted() && conversationId.equals(c.getConversationId()) && channelId.equals(c.getParentChannelId())) return copyOf(c);
            }
            NotificationChannel c = sChannels.get(channelId);
            return c == null || c.isDeleted() ? null : copyOf(c);
        }
    }
    public List<NotificationChannel> getNotificationChannels() {
        synchronized (sLock) {
            load();
            ArrayList<NotificationChannel> out = new ArrayList<>();
            for (NotificationChannel c : sChannels.values()) if (!c.isDeleted()) out.add(copyOf(c));
            return out;
        }
    }
    public void deleteNotificationChannel(String channelId) {
        synchronized (sLock) {
            load();
            NotificationChannel c = sChannels.get(channelId);
            if (c != null && delete(c)) save();
        }
    }
    /** The group with the app's channels in it (Android 9), null where there is none. */
    public NotificationChannelGroup getNotificationChannelGroup(String channelGroupId) {
        synchronized (sLock) {
            load();
            NotificationChannelGroup g = channelGroupId == null ? null : sGroups.get(channelGroupId);
            return g == null ? null : groupOut(g);
        }
    }
    /** Every group with the app's channels in it, those with none too (Android's). */
    public List<NotificationChannelGroup> getNotificationChannelGroups() {
        synchronized (sLock) {
            load();
            ArrayList<NotificationChannelGroup> out = new ArrayList<>();
            for (NotificationChannelGroup g : sGroups.values()) out.add(groupOut(g));
            return out;
        }
    }
    /** The group goes, and its channels are deleted (Android's: nothing for a group that is not there). */
    public void deleteNotificationChannelGroup(String groupId) {
        synchronized (sLock) {
            load();
            if (groupId == null || sGroups.remove(groupId) == null) return;
            for (NotificationChannel c : sChannels.values()) if (groupId.equals(c.getGroup())) delete(c);
            save();
        }
    }

    // ---- The app's channels and groups, as Android's system keeps them (see the note at the top)

    /* The file in the app's data folder, its root element, and how long a deleted channel is kept (Android's 30 days). */
    private static final String FILE = ".megler_notification_channels.xml", TAG_ROOT = "notification-channels";
    private static final long DELETED_KEPT_MS = 30L * 24 * 60 * 60 * 1000;
    private static final Object sLock = new Object();
    /* By id, in the order they were made: deleted channels among them. Null until the file is read (load). */
    private static LinkedHashMap<String, NotificationChannel> sChannels;
    private static LinkedHashMap<String, NotificationChannelGroup> sGroups;
    private static final java.util.concurrent.atomic.AtomicBoolean sWritePending = new java.util.concurrent.atomic.AtomicBoolean();
    private static java.util.concurrent.ExecutorService sWriter;

    /** A copy of a channel that shares nothing with it, as Android's binder hands one over: through a Parcel. */
    private static NotificationChannel copyOf(NotificationChannel c) {
        android.os.Parcel p = android.os.Parcel.obtain();
        try {
            c.writeToParcel(p, 0);
            p.setDataPosition(0);
            return NotificationChannel.CREATOR.createFromParcel(p);
        } finally {
            p.recycle();
        }
    }

    /** The app's channel made or changed as Android's PreferencesHelper.createNotificationChannel does it for an app (sLock held). */
    private static boolean create(NotificationChannel channel) {
        Objects.requireNonNull(channel, "channel in list is null");
        Objects.requireNonNull(channel.getId());
        NotificationChannel existing = sChannels.get(channel.getId());
        if (existing != null) {
            // Only what an app may change of a channel that is there.
            boolean changed = false;
            if (existing.isDeleted()) {
                existing.setDeleted(false);
                existing.setDeletedTimeMs(-1);
                changed = true;
            }
            String name = channel.getName() == null ? null : channel.getName().toString();
            if (!Objects.equals(name, existing.getName() == null ? null : existing.getName().toString())) {
                existing.setName(name);
                changed = true;
            }
            if (!Objects.equals(channel.getDescription(), existing.getDescription())) {
                existing.setDescription(channel.getDescription());
                changed = true;
            }
            if (channel.isBlockable() != existing.isBlockable()) {
                existing.setBlockable(channel.isBlockable());
                changed = true;
            }
            if (channel.getGroup() != null && existing.getGroup() == null) {
                existing.setGroup(channel.getGroup());
                changed = true;
            }
            // Downwards, while the user has changed nothing of it.
            if (existing.getUserLockedFields() == 0 && channel.getImportance() < existing.getImportance()) {
                existing.setImportance(channel.getImportance());
                changed = true;
            }
            if (existing.getOriginalImportance() == IMPORTANCE_UNSPECIFIED) {
                existing.setOriginalImportance(channel.getImportance());
                changed = true;
            }
            return changed;
        }
        NotificationChannel c = copyOf(channel);
        // The fields an app may not set, as an app's new channel has them (no access to Do Not Disturb's policy).
        c.setBypassDnd(false);
        c.setLockscreenVisibility(VISIBILITY_NO_OVERRIDE);
        c.setAllowBubbles(NotificationChannel.DEFAULT_ALLOW_BUBBLE);
        c.setImportantConversation(false);
        c.unlockFields(c.getUserLockedFields());
        c.setImportanceLockedByCriticalDeviceFunction(false);
        c.setOriginalImportance(c.getImportance());
        sChannels.put(c.getId(), c);
        return true;
    }

    /** A channel marked deleted, its time kept (sLock held): whether it was not already. */
    private static boolean delete(NotificationChannel c) {
        if (c.isDeleted()) return false;
        c.setDeleted(true);
        c.setDeletedTimeMs(System.currentTimeMillis());
        return true;
    }

    /** The app's group made or changed as Android's PreferencesHelper.createNotificationChannelGroup does it for an app (sLock held). */
    private static boolean create(NotificationChannelGroup group) {
        Objects.requireNonNull(group);
        Objects.requireNonNull(group.getId());
        NotificationChannelGroup old = sGroups.get(group.getId());
        NotificationChannelGroup g = group.clone();
        g.setChannels(new ArrayList<>());
        // An app may not block a group, nor unblock one, nor change what the user locked of one that is there.
        g.setBlocked(old != null && old.isBlocked());
        if (old != null) {
            g.unlockFields(g.getUserLockedFields());
            g.lockFields(old.getUserLockedFields());
        }
        sGroups.put(g.getId(), g);
        return !g.equals(old);
    }

    /** A group handed out: a copy with copies of its channels that are not deleted (sLock held). */
    private static NotificationChannelGroup groupOut(NotificationChannelGroup g) {
        NotificationChannelGroup out = g.clone();
        out.setChannels(new ArrayList<>());
        for (NotificationChannel c : sChannels.values()) if (!c.isDeleted() && g.getId().equals(c.getGroup())) out.addChannel(copyOf(c));
        return out;
    }

    private static java.io.File file() { return new java.io.File(megler.Native.dataDir(), FILE); }

    /** The channels and groups of the app's last runs, read the first time they are asked for (sLock held). */
    private static void load() {
        if (sChannels != null) return;
        sChannels = new LinkedHashMap<>();
        sGroups = new LinkedHashMap<>();
        java.io.File f;
        try {
            f = file();
        } catch (Throwable t) {
            return;
        }
        if (!f.isFile()) return;
        int deleted = 0, forgotten = 0;
        try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
            org.xmlpull.v1.XmlPullParser p = android.util.Xml.newPullParser();
            p.setInput(in, "UTF-8");
            long boundary = System.currentTimeMillis() - DELETED_KEPT_MS;
            for (int e = p.getEventType(); e != org.xmlpull.v1.XmlPullParser.END_DOCUMENT; e = p.next()) {
                if (e != org.xmlpull.v1.XmlPullParser.START_TAG) continue;
                String id = p.getAttributeValue(null, "id");
                if (id == null) continue;
                if ("channelGroup".equals(p.getName())) {
                    NotificationChannelGroup g = new NotificationChannelGroup(id, p.getAttributeValue(null, "name"));
                    g.populateFromXml(p);
                    sGroups.put(id, g);
                } else if ("channel".equals(p.getName())) {
                    int importance = IMPORTANCE_UNSPECIFIED;
                    String imp = p.getAttributeValue(null, "importance");
                    try { if (imp != null) importance = Integer.parseInt(imp); } catch (NumberFormatException x) {}
                    NotificationChannel c = new NotificationChannel(id, p.getAttributeValue(null, "name"), importance);
                    c.populateFromXml(p);
                    // A channel written without its sound's attributes had none (a silent one's setSound(null, null)). Android's
                    // reader gives it a notification's, which an app sees only after the phone restarts: the app's next run is
                    // no restart of the phone, and gets the channel as it was.
                    if (p.getAttributeValue(null, "usage") == null) c.setSound(c.getSound(), null);
                    if (c.isDeleted() && c.getDeletedTimeMs() <= boundary) {
                        forgotten++;
                        continue;
                    }
                    if (c.isDeleted()) deleted++;
                    sChannels.put(id, c);
                }
            }
        } catch (Exception e) {
            android.util.Log.w("Megler", "notification channels: " + f.getName() + " read only in part: " + e);
        }
        android.util.Log.i("Megler", "notification channels: " + (sChannels.size() - deleted) + " kept from the app's last run and " + deleted + " deleted, "
                + sGroups.size() + (sGroups.size() == 1 ? " group" : " groups")
                + (forgotten > 0 ? "; " + forgotten + " deleted more than 30 days ago forgotten" : ""));
    }

    /** A change, written to the file on a thread of its own: one write for a burst of changes (sLock held). */
    private static void save() {
        if (!sWritePending.compareAndSet(false, true)) return;
        if (sWriter == null) {
            sWriter = java.util.concurrent.Executors.newSingleThreadExecutor(r -> {
                Thread t = new Thread(r, "NotificationChannelsWriter");
                t.setDaemon(true);
                return t;
            });
        }
        sWriter.execute(NotificationManager::write);
    }

    /** The channels and groups as they are now, into a file beside the old one, which it then takes the place of. */
    private static void write() {
        sWritePending.set(false);
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        try {
            synchronized (sLock) {
                org.xmlpull.v1.XmlSerializer out = android.util.Xml.newSerializer();
                out.setOutput(bytes, "UTF-8");
                out.startDocument("UTF-8", true);
                out.startTag(null, TAG_ROOT);
                out.attribute(null, "version", "1");
                for (NotificationChannelGroup g : sGroups.values()) g.writeXml(out);
                for (NotificationChannel c : sChannels.values()) c.writeXml(out);
                out.endTag(null, TAG_ROOT);
                out.endDocument();
            }
            java.io.File f = file(), next = new java.io.File(f.getPath() + ".new");
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(next)) {
                o.write(bytes.toByteArray());
                o.getFD().sync();
            }
            if (!next.renameTo(f)) {
                next.delete();
                throw new java.io.IOException("the new file could not take the old one's place");
            }
        } catch (Throwable t) {
            android.util.Log.w("Megler", "notification channels: not written: " + t);
        }
    }

    public boolean areNotificationsEnabled() { return true; }
    public boolean areNotificationsPaused() { return false; }
    public int getImportance() { return IMPORTANCE_DEFAULT; }
    public android.service.notification.StatusBarNotification[] getActiveNotifications() { return new android.service.notification.StatusBarNotification[0]; }
    public boolean isNotificationPolicyAccessGranted() { return false; }
    public int getCurrentInterruptionFilter() { return INTERRUPTION_FILTER_ALL; }
    public boolean canNotifyAsPackage(String p) { return false; }
    public boolean canUseFullScreenIntent() { return true; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public NotificationManager() {}
    private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
    public static final java.lang.String ACTION_AUTOMATIC_ZEN_RULE = "android.app.action.AUTOMATIC_ZEN_RULE";
    public static final java.lang.String ACTION_AUTOMATIC_ZEN_RULE_STATUS_CHANGED = "android.app.action.AUTOMATIC_ZEN_RULE_STATUS_CHANGED";
    public static final java.lang.String ACTION_CLOSE_NOTIFICATION_HANDLER_PANEL = "android.app.action.CLOSE_NOTIFICATION_HANDLER_PANEL";
    public static final java.lang.String ACTION_CONSOLIDATED_NOTIFICATION_POLICY_CHANGED = "android.app.action.CONSOLIDATED_NOTIFICATION_POLICY_CHANGED";
    public static final java.lang.String ACTION_EFFECTS_SUPPRESSOR_CHANGED = "android.os.action.ACTION_EFFECTS_SUPPRESSOR_CHANGED";
    public static final java.lang.String ACTION_INTERRUPTION_FILTER_CHANGED = "android.app.action.INTERRUPTION_FILTER_CHANGED";
    public static final java.lang.String ACTION_INTERRUPTION_FILTER_CHANGED_INTERNAL = "android.app.action.INTERRUPTION_FILTER_CHANGED_INTERNAL";
    public static final java.lang.String ACTION_NOTIFICATION_CHANNEL_BLOCK_STATE_CHANGED = "android.app.action.NOTIFICATION_CHANNEL_BLOCK_STATE_CHANGED";
    public static final java.lang.String ACTION_NOTIFICATION_CHANNEL_GROUP_BLOCK_STATE_CHANGED = "android.app.action.NOTIFICATION_CHANNEL_GROUP_BLOCK_STATE_CHANGED";
    public static final java.lang.String ACTION_NOTIFICATION_LISTENER_ENABLED_CHANGED = "android.app.action.NOTIFICATION_LISTENER_ENABLED_CHANGED";
    public static final java.lang.String ACTION_NOTIFICATION_POLICY_CHANGED = "android.app.action.NOTIFICATION_POLICY_CHANGED";
    public static final java.lang.String ACTION_OPEN_NOTIFICATION_HANDLER_PANEL = "android.app.action.OPEN_NOTIFICATION_HANDLER_PANEL";
    public static final java.lang.String ACTION_TOGGLE_NOTIFICATION_HANDLER_PANEL = "android.app.action.TOGGLE_NOTIFICATION_HANDLER_PANEL";
    public static final java.lang.String ACTION_ZEN_CONFIGURATION_CHANGED_INTERNAL = "android.app.action.ZEN_CONFIGURATION_CHANGED_INTERNAL";
    public static final int AUTOMATIC_RULE_STATUS_ACTIVATED = 4;
    public static final int AUTOMATIC_RULE_STATUS_DEACTIVATED = 5;
    public static final int AUTOMATIC_RULE_STATUS_DISABLED = 2;
    public static final int AUTOMATIC_RULE_STATUS_ENABLED = 1;
    public static final int AUTOMATIC_RULE_STATUS_REMOVED = 3;
    public static final int AUTOMATIC_RULE_STATUS_UNKNOWN = -1;
    public static final int BUBBLE_PREFERENCE_ALL = 1;
    public static final int BUBBLE_PREFERENCE_NONE = 0;
    public static final int BUBBLE_PREFERENCE_SELECTED = 2;
    public static final java.lang.String EXTRA_AUTOMATIC_RULE_ID = "android.app.extra.AUTOMATIC_RULE_ID";
    public static final java.lang.String EXTRA_AUTOMATIC_ZEN_RULE_ID = "android.app.extra.AUTOMATIC_ZEN_RULE_ID";
    public static final java.lang.String EXTRA_AUTOMATIC_ZEN_RULE_STATUS = "android.app.extra.AUTOMATIC_ZEN_RULE_STATUS";
    public static final java.lang.String EXTRA_BLOCKED_STATE = "android.app.extra.BLOCKED_STATE";
    public static final java.lang.String EXTRA_NOTIFICATION_CHANNEL_GROUP_ID = "android.app.extra.NOTIFICATION_CHANNEL_GROUP_ID";
    public static final java.lang.String EXTRA_NOTIFICATION_CHANNEL_ID = "android.app.extra.NOTIFICATION_CHANNEL_ID";
    public static final java.lang.String EXTRA_NOTIFICATION_POLICY = "android.app.extra.NOTIFICATION_POLICY";
    public static int MAX_SERVICE_COMPONENT_NAME_LENGTH;
    public static final java.lang.String META_DATA_AUTOMATIC_RULE_TYPE = "android.service.zen.automatic.ruleType";
    public static final java.lang.String META_DATA_RULE_INSTANCE_LIMIT = "android.service.zen.automatic.ruleInstanceLimit";
    public static final long SET_LISTENER_ACCESS_GRANTED_IS_USER_AWARE = 302563478L;
    public static final int VISIBILITY_NO_OVERRIDE = -1000;
    public NotificationManager(android.content.Context p0) { this(); }
    public static android.app.NotificationManager from(android.content.Context p0) { return null; }
    public static android.app.INotificationManager getService() { return null; }
    public static void invalidateNotificationChannelCache() {}
    public static void invalidateNotificationChannelGroupCache() {}
    public static int zenModeFromInterruptionFilter(int p0, int p1) { return 0; }
    public static int zenModeToInterruptionFilter(int p0) { return 0; }
    public java.lang.String addAutomaticZenRule(android.app.AutomaticZenRule p0) { return null; }
    public java.lang.String addAutomaticZenRule(android.app.AutomaticZenRule p0, boolean p1) { return null; }
    public void allowAssistantAdjustment(java.lang.String p0) {}
    public boolean areAutomaticZenRulesUserManaged() { return false; }
    public boolean areBubblesAllowed() { return false; }
    public boolean areBubblesEnabled() { return false; }
    public boolean canPostPromotedNotifications() { return false; }
    public void cancelAsPackage(java.lang.String p0, java.lang.String p1, int p2) {}
    public void cancelAsUser(java.lang.String p0, int p1, android.os.UserHandle p2) {}
    public void cleanUpCallersAfter(long p0) {}
    public void disallowAssistantAdjustment(java.lang.String p0) {}
    public java.util.List getAllowedAssistantAdjustments() { return new java.util.ArrayList(); }
    public android.content.ComponentName getAllowedNotificationAssistant() { return null; }
    public android.app.AutomaticZenRule getAutomaticZenRule(java.lang.String p0) { return null; }
    public int getAutomaticZenRuleState(java.lang.String p0) { return 0; }
    public java.util.Map getAutomaticZenRules() { return new java.util.HashMap(); }
    public int getBubblePreference() { return 0; }
    public android.app.NotificationManager.Policy getConsolidatedNotificationPolicy() { return null; }
    public android.service.notification.ZenPolicy getDefaultZenPolicy() { return null; }
    public android.content.ComponentName getEffectsSuppressor() { return null; }
    public java.util.List getEnabledNotificationListenerPackages() { return new java.util.ArrayList(); }
    public java.util.List getEnabledNotificationListeners() { return new java.util.ArrayList(); }
    public java.util.List getEnabledNotificationListeners(int p0) { return new java.util.ArrayList(); }
    public java.lang.String getNotificationDelegate() { return (java.lang.String) megler.Fill.get(meglerFill, "NotificationDelegate"); }
    public android.app.NotificationManager.Policy getNotificationPolicy() { return (android.app.NotificationManager.Policy) megler.Fill.get(meglerFill, "NotificationPolicy"); }
    public int getRuleInstanceCount(android.content.ComponentName p0) { return 0; }
    public java.util.Set getUnsupportedAdjustmentTypes() { return new java.util.HashSet(); }
    public int getZenMode() { return (megler.Fill.get(meglerFill, "ZenMode") instanceof Integer ? (Integer) megler.Fill.get(meglerFill, "ZenMode") : 0); }
    public boolean hasEnabledNotificationListener(java.lang.String p0, android.os.UserHandle p1) { return false; }
    public boolean isNotificationAssistantAccessGranted(android.content.ComponentName p0) { return false; }
    public boolean isNotificationListenerAccessGranted(android.content.ComponentName p0) { return false; }
    public boolean isNotificationPolicyAccessGrantedForPackage(java.lang.String p0) { return false; }
    public boolean isSystemConditionProviderEnabled(java.lang.String p0) { return false; }
    public boolean matchesCallFilter(android.net.Uri p0) { return false; }
    public boolean matchesCallFilter(android.os.Bundle p0) { return false; }
    public void notifyAsPackage(java.lang.String p0, java.lang.String p1, int p2, android.app.Notification p3) {}
    public void notifyAsUser(java.lang.String p0, int p1, android.app.Notification p2, android.os.UserHandle p3) {}
    public void registerCallNotificationEventListener(java.lang.String p0, android.os.UserHandle p1, java.util.concurrent.Executor p2, android.app.NotificationManager.CallNotificationEventListener p3) {}
    public boolean removeAutomaticZenRule(java.lang.String p0) { return false; }
    public boolean removeAutomaticZenRule(java.lang.String p0, boolean p1) { return false; }
    public boolean removeAutomaticZenRules(java.lang.String p0) { return false; }
    public boolean removeAutomaticZenRules(java.lang.String p0, boolean p1) { return false; }
    protected android.app.INotificationManager service() { return null; }
    public void setAssistantAdjustmentKeyTypeState(int p0, boolean p1) {}
    public void setAssistantClassificationTypeState(int p0, boolean p1) {}
    public void setAutomaticZenRuleState(java.lang.String p0, android.service.notification.Condition p1) {}
    public void setCanPostPromotedNotifications(java.lang.String p0, int p1, boolean p2) {}
    public void setChannelCachesToTestMode() {}
    public void setInterruptionFilter(int p0) {}
    public void setInterruptionFilter(int p0, boolean p1) {}
    public void setManualZenRuleDeviceEffects(android.service.notification.ZenDeviceEffects p0) {}
    public void setNotificationAssistantAccessGranted(android.content.ComponentName p0, boolean p1) {}
    public void setNotificationDelegate(java.lang.String p0) { meglerFill = megler.Fill.put(meglerFill, "NotificationDelegate", p0); }
    public void setNotificationListenerAccessGranted(android.content.ComponentName p0, boolean p1) {}
    public void setNotificationListenerAccessGranted(android.content.ComponentName p0, boolean p1, boolean p2) {}
    public void setNotificationListenerAccessGrantedForUser(android.content.ComponentName p0, int p1, boolean p2) {}
    public void setNotificationPolicy(android.app.NotificationManager.Policy p0) { meglerFill = megler.Fill.put(meglerFill, "NotificationPolicy", p0); }
    public void setNotificationPolicy(android.app.NotificationManager.Policy p0, boolean p1) {}
    public void setNotificationPolicyAccessGranted(java.lang.String p0, boolean p1) {}
    public void setToastRateLimitingEnabled(boolean p0) {}
    public void setZenMode(int p0, android.net.Uri p1, java.lang.String p2) {}
    public void setZenMode(int p0, android.net.Uri p1, java.lang.String p2, boolean p3) {}
    public boolean shouldHideSilentStatusBarIcons() { return false; }
    public void silenceNotificationSound() {}
    public void unregisterCallNotificationEventListener(android.app.NotificationManager.CallNotificationEventListener p0) {}
    public boolean updateAutomaticZenRule(java.lang.String p0, android.app.AutomaticZenRule p1) { return false; }
    public boolean updateAutomaticZenRule(java.lang.String p0, android.app.AutomaticZenRule p1, boolean p2) { return false; }
    public void updateNotificationChannel(java.lang.String p0, int p1, android.app.NotificationChannel p2) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public interface CallNotificationEventListener {
        void onCallNotificationPosted(java.lang.String p0, android.os.UserHandle p1);
        void onCallNotificationRemoved(java.lang.String p0, android.os.UserHandle p1);
    }
    public static class Policy implements android.os.Parcelable {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public static int[] ALL_PRIORITY_CATEGORIES;
        public static final int CONVERSATION_SENDERS_ANYONE = 1;
        public static final int CONVERSATION_SENDERS_IMPORTANT = 2;
        public static final int CONVERSATION_SENDERS_NONE = 3;
        public static final int CONVERSATION_SENDERS_UNSET = -1;
        public static android.os.Parcelable.Creator CREATOR;
        public static final int PRIORITY_CATEGORY_ALARMS = 32;
        public static final int PRIORITY_CATEGORY_CALLS = 8;
        public static final int PRIORITY_CATEGORY_CONVERSATIONS = 256;
        public static final int PRIORITY_CATEGORY_EVENTS = 2;
        public static final int PRIORITY_CATEGORY_MEDIA = 64;
        public static final int PRIORITY_CATEGORY_MESSAGES = 4;
        public static final int PRIORITY_CATEGORY_REMINDERS = 1;
        public static final int PRIORITY_CATEGORY_REPEAT_CALLERS = 16;
        public static final int PRIORITY_CATEGORY_SYSTEM = 128;
        public static final int PRIORITY_SENDERS_ANY = 0;
        public static final int PRIORITY_SENDERS_CONTACTS = 1;
        public static final int PRIORITY_SENDERS_STARRED = 2;
        public static final int STATE_HAS_PRIORITY_CHANNELS = 1;
        public static final int STATE_PRIORITY_CHANNELS_BLOCKED = 2;
        public static final int STATE_UNSET = -1;
        public static final int SUPPRESSED_EFFECTS_UNSET = -1;
        public static final int SUPPRESSED_EFFECT_AMBIENT = 128;
        public static final int SUPPRESSED_EFFECT_BADGE = 64;
        public static final int SUPPRESSED_EFFECT_FULL_SCREEN_INTENT = 4;
        public static final int SUPPRESSED_EFFECT_LIGHTS = 8;
        public static final int SUPPRESSED_EFFECT_NOTIFICATION_LIST = 256;
        public static final int SUPPRESSED_EFFECT_PEEK = 16;
        public static final int SUPPRESSED_EFFECT_SCREEN_OFF = 1;
        public static final int SUPPRESSED_EFFECT_SCREEN_ON = 2;
        public static final int SUPPRESSED_EFFECT_STATUS_BAR = 32;
        public int priorityCallSenders;
        public int priorityCategories;
        public int priorityConversationSenders;
        public int priorityMessageSenders;
        public int state;
        public int suppressedVisualEffects;
        public Policy(int p0, int p1, int p2) {}
        public Policy(int p0, int p1, int p2, int p3) {}
        public Policy(int p0, int p1, int p2, int p3, int p4) {}
        public Policy(int p0, int p1, int p2, int p3, int p4, int p5) {}
        public Policy(android.os.Parcel p0) {}
        public static boolean areAllVisualEffectsSuppressed(int p0) { return false; }
        public static java.lang.String conversationSendersToString(int p0) { return null; }
        public static int getAllSuppressedVisualEffects() { return 0; }
        public static int policyState(boolean p0, boolean p1) { return 0; }
        public static java.lang.String priorityCategoriesToString(int p0) { return null; }
        public static java.lang.String prioritySendersToString(int p0) { return null; }
        public static java.lang.String suppressedEffectsToString(int p0) { return null; }
        public boolean allowAlarms() { return false; }
        public boolean allowCalls() { return false; }
        public int allowCallsFrom() { return 0; }
        public boolean allowConversations() { return false; }
        public int allowConversationsFrom() { return 0; }
        public boolean allowEvents() { return false; }
        public boolean allowMedia() { return false; }
        public boolean allowMessages() { return false; }
        public int allowMessagesFrom() { return 0; }
        public boolean allowPriorityChannels() { return false; }
        public boolean allowReminders() { return false; }
        public boolean allowRepeatCallers() { return false; }
        public boolean allowSystem() { return false; }
        public android.app.NotificationManager.Policy copy() { return new android.app.NotificationManager.Policy(); }
        public int describeContents() { return 0; }
        public void dumpDebug(android.util.proto.ProtoOutputStream p0, long p1) {}
        public boolean hasPriorityChannels() { return false; }
        public boolean showAmbient() { return false; }
        public boolean showBadges() { return false; }
        public boolean showFullScreenIntents() { return false; }
        public boolean showInNotificationList() { return false; }
        public boolean showLights() { return false; }
        public boolean showPeeking() { return false; }
        public boolean showStatusBarIcons() { return false; }
        public void writeToParcel(android.os.Parcel p0, int p1) {}
        Policy() { this((android.os.Parcel) null); }
    }
    // ---- end of generated nested classes
}
