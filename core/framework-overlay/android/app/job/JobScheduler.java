// Overlay of core/framework/src/android/app/job/JobScheduler.java (tools/build-framework.sh).
// It differs in running the app's jobs (Impl), where the original kept them and never ran them, and in keeping them once for
// the app, whichever instance of the scheduler is asked.
//
// Why. Jobs were kept and never run, and a job's info kept its id and extras only: Play Store's download service has its
// downloads done by jobs of its DownloadJobService (needing a network), and asks the pending job's service by its class
// (getPendingJob(...).getService(), which was null). Megler runs a job as Android's job scheduler does once its constraints
// hold, network ones taken to hold while the phone has a network: its service made as the app's others are (megler.Services:
// in its process's scope, megler.Processes), on the main thread, after its minimum latency.
//
// A job's service that is not a JobService but a Service whose onBind returns a JobServiceEngine's binder (AndroidX's
// JobIntentService, on API 26 and later) is run through its engine, as Android's scheduler binds it and runs it through that
// binder (JobServiceEngine, overlaid): it was dropped as "no job service of the app's".
//
// A job that waits for a change of content (JobInfo.Builder.addTriggerContentUri) is held until one is told, as Android's
// ContentObserverController holds it: it observes its URIs (their descendants where the trigger's flag says so) through
// ContentResolver, as any observer of the app's does, so an app with no such job pays nothing; a change told is kept (50 URIs
// at the most, and every authority), the job runs once no further change has come for its update delay (10 s when not set,
// 500 ms at the least) or its maximum delay since the first has passed (2 minutes when not set, 1 s at the least), and its
// parameters say what changed (JobParameters.getTriggeredContentUris, getTriggeredContentAuthorities). A change told while
// the job runs goes to the job scheduled under its id meanwhile, which then runs at once; a job that asks to be run again
// is told its changes again; a job with an override deadline runs at it, changed or not. Before, such a job ran at
// once: Files by Google's WorkManager work that waits for content to change ran, was enqueued again by its worker and ran
// again, every two seconds for as long as the app lived (job ids from 30 to 1246 in an hour).
//
// Not here: jobs persisted across the app's runs (setPersisted), constraints other than the time and content (charging, idle,
// a network of a kind: taken to hold), and the deadline of a job without content triggers: such a job runs once it may.
//
// Everything else is the original's, unchanged.
package android.app.job;
public abstract class JobScheduler {
    public static final int RESULT_FAILURE = 0, RESULT_SUCCESS = 1;
    public abstract int schedule(JobInfo job); public abstract int enqueue(JobInfo job, JobWorkItem w); public abstract void cancel(int id); public abstract void cancelAll();
    public abstract java.util.List<JobInfo> getAllPendingJobs(); public abstract JobInfo getPendingJob(int id);
    /** A job scheduled: its info, the work enqueued for it, and its run when it runs. */
    static final class Run {
        final JobInfo info;
        final java.util.ArrayDeque<JobWorkItem> work = new java.util.ArrayDeque<>();
        JobService service;
        JobServiceEngine engine;           /* or the engine of a service that has one (JobIntentService) */
        JobParameters params;
        Runnable planned;
        boolean running;
        int failures, stopReason;
        /* Of a job that waits for a change of content (JobInfo.getTriggerContentUris), all under sJobs' lock: its observers
           while it is watched; what was told changed since its last run began; whether it may run for it (the delay after a
           change has passed, or changes were handed to it); the two delays' timers; and the uptimes its minimum latency ends
           at and its deadline is at (0: none). */
        android.database.ContentObserver[] observers;
        java.util.LinkedHashSet<android.net.Uri> changedUris;
        java.util.LinkedHashSet<String> changedAuthorities;
        boolean triggered;
        Runnable triggerSoon, triggerLatest;
        long earliest, deadline;
        Run(JobInfo info) { this.info = info; }
        boolean content() { return info.getTriggerContentUris() != null; }
    }

    /**
     * The app's jobs, every instance of the scheduler the same ones (each getSystemService makes one), run as Android's job
     * scheduler runs them: on the main thread, once the minimum latency has passed (a network asked for is taken to be there),
     * by onStartJob of the job's service; the job ends when onStartJob answers false, at jobFinished, or when dequeueWork has no
     * more; then again after its backoff if it asked to be rescheduled, after its interval if it repeats, at once if work was
     * enqueued meanwhile. Scheduling a job again, or cancelling it, stops it if it runs (onStopJob). A job with content triggers
     * waits besides for a change of one of its URIs (watch, changed, trigger).
     */
    public static final class Impl extends JobScheduler {
        private static final java.util.HashMap<Integer, Run> sJobs = new java.util.HashMap<>();
        private static android.os.Handler sHandler;
        private static synchronized android.os.Handler handler() {
            if (sHandler == null) sHandler = new android.os.Handler(android.os.Looper.getMainLooper());
            return sHandler;
        }
        public int schedule(JobInfo j) {
            if (j == null || j.getService() == null) return RESULT_FAILURE;
            Run old, run = new Run(j);
            boolean content = run.content();
            long now = android.os.SystemClock.uptimeMillis();
            run.earliest = now + Math.max(0, j.getMinLatencyMillis());
            run.deadline = content && j.getMaxExecutionDelayMillis() > 0 ? now + j.getMaxExecutionDelayMillis() : 0;
            synchronized (sJobs) {
                old = sJobs.put(j.getId(), run);
                /* Changes told since the job of this id began its run are this one's to run for, as on Android. */
                if (old != null && content && old.changedAuthorities != null) {
                    run.changedUris = old.changedUris; run.changedAuthorities = old.changedAuthorities;
                    old.changedUris = null; old.changedAuthorities = null;
                    run.triggered = true;
                }
            }
            if (old != null) stop(old, 1 /* STOP_REASON_CANCELLED_BY_APP */);
            if (!content) { plan(run, j.getMinLatencyMillis()); return RESULT_SUCCESS; }
            watch(run);
            if (run.triggered) plan(run, j.getMinLatencyMillis());
            else if (run.deadline != 0) plan(run, j.getMaxExecutionDelayMillis());   /* at its deadline it runs, changed or not */
            android.util.Log.i("Megler", "job " + j.getId() + " (" + j.getService().getShortClassName() + ") waits for a change of its content, "
                               + j.getTriggerContentUris().length + " URI(s)" + (run.triggered ? ": changes told before are its own" : ""));
            return RESULT_SUCCESS;
        }

        private static final int MAX_URIS_REPORTED = 50;   /* Android's ContentObserverController's */
        private static long updateDelay(JobInfo j) { long d = j.getTriggerContentUpdateDelay(); return d < 0 ? 10000 : Math.max(d, 500); }
        private static long maxDelay(JobInfo j) { long d = j.getTriggerContentMaxDelay(); return d < 0 ? 120000 : Math.max(d, 1000); }

        /** The job's content URIs observed: the app's ContentResolver.notifyChange of one (or of a descendant, where asked) is told to changed(). */
        private static void watch(final Run run) {
            JobInfo.TriggerContentUri[] triggers = run.info.getTriggerContentUris();
            android.database.ContentObserver[] observers = new android.database.ContentObserver[triggers.length];
            android.content.ContentResolver resolver = android.content.ContentResolver.self();
            synchronized (sJobs) { if (sJobs.get(run.info.getId()) != run) return; run.observers = observers; }   /* watched from here on */
            for (int i = 0; i < triggers.length; i++) {
                final android.net.Uri uri = triggers[i] != null ? triggers[i].getUri() : null;
                if (uri == null) continue;
                observers[i] = new android.database.ContentObserver(null) {
                    @Override public void onChange(boolean self, android.net.Uri u) { changed(run, u != null ? u : uri); }
                };
                resolver.registerContentObserver(uri, (triggers[i].getFlags() & JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS) != 0, observers[i]);
            }
            boolean gone;
            synchronized (sJobs) { gone = run.observers != observers; }
            if (gone) for (android.database.ContentObserver o : observers) if (o != null) resolver.unregisterContentObserver(o);   /* cancelled meanwhile */
        }
        /** The job is watched no more (cancelled, replaced, or ended for good), and its delays' timers are stopped. */
        private static void unwatch(Run run) {
            android.database.ContentObserver[] observers;
            synchronized (sJobs) { observers = run.observers; run.observers = null; untime(run); }
            if (observers == null) return;
            android.content.ContentResolver resolver = android.content.ContentResolver.self();
            for (android.database.ContentObserver o : observers) if (o != null) resolver.unregisterContentObserver(o);
        }
        /* Under sJobs' lock. */
        private static void untime(Run run) {
            if (run.triggerSoon != null) handler().removeCallbacks(run.triggerSoon);
            if (run.triggerLatest != null) handler().removeCallbacks(run.triggerLatest);
            run.triggerSoon = run.triggerLatest = null;
        }
        /**
         * A change of one of the job's URIs was told (on the thread that told it): kept for the job's parameters; and a job
         * that waits runs once no other has come for its update delay, or its maximum delay after the first. One that runs
         * keeps it for the job scheduled in its stead, or for its own next run if it asks for one.
         */
        static void changed(final Run run, android.net.Uri uri) {
            synchronized (sJobs) {
                if (run.observers == null) return;
                if (run.changedUris == null) run.changedUris = new java.util.LinkedHashSet<>();
                if (run.changedUris.size() < MAX_URIS_REPORTED) run.changedUris.add(uri);
                if (run.changedAuthorities == null) run.changedAuthorities = new java.util.LinkedHashSet<>();
                run.changedAuthorities.add(uri.getAuthority());
                if (run.running || run.triggered) return;
                if (run.triggerLatest == null) {
                    run.triggerLatest = () -> trigger(run);
                    handler().postDelayed(run.triggerLatest, maxDelay(run.info));
                }
                if (run.triggerSoon != null) handler().removeCallbacks(run.triggerSoon);
                else run.triggerSoon = () -> trigger(run);
                handler().postDelayed(run.triggerSoon, updateDelay(run.info));
            }
        }
        /** The delay after a change has passed: the job may run, once its minimum latency (or its backoff) has passed too. */
        private static void trigger(Run run) {
            synchronized (sJobs) {
                if (run.observers == null || run.triggered || run.running || sJobs.get(run.info.getId()) != run) return;
                run.triggered = true;
                untime(run);
            }
            plan(run, run.earliest - android.os.SystemClock.uptimeMillis());
        }
        public int enqueue(JobInfo j, JobWorkItem w) {
            if (j == null || j.getService() == null) return RESULT_FAILURE;
            Run run;
            synchronized (sJobs) {
                run = sJobs.get(j.getId());
                if (run != null && j.getService().equals(run.info.getService())) {
                    if (w != null) run.work.add(w);
                    if (run.running || run.planned != null || (run.content() && !run.triggered)) return RESULT_SUCCESS;   /* or it waits for its content */
                } else {
                    run = null;
                }
            }
            if (run == null) {
                int result = schedule(j);
                synchronized (sJobs) { Run made = sJobs.get(j.getId()); if (made != null && w != null) made.work.add(w); }
                return result;
            }
            plan(run, 0);
            return RESULT_SUCCESS;
        }
        public void cancel(int id) { Run run; synchronized (sJobs) { run = sJobs.remove(id); } if (run != null) stop(run, 1); }
        public void cancelAll() {
            java.util.ArrayList<Run> all;
            synchronized (sJobs) { all = new java.util.ArrayList<>(sJobs.values()); sJobs.clear(); }
            for (Run run : all) stop(run, 1);
        }
        public java.util.List<JobInfo> getAllPendingJobs() {
            java.util.ArrayList<JobInfo> l = new java.util.ArrayList<>();
            synchronized (sJobs) { for (Run run : sJobs.values()) l.add(run.info); }
            return l;
        }
        public JobInfo getPendingJob(int id) { synchronized (sJobs) { Run run = sJobs.get(id); return run != null ? run.info : null; } }

        private static void plan(Run run, long delay) {
            Runnable r = () -> start(run);
            synchronized (sJobs) {
                if (run.planned != null) handler().removeCallbacks(run.planned);
                run.planned = r;
            }
            handler().postDelayed(r, Math.max(0, delay));
        }
        private static boolean current(Run run) { synchronized (sJobs) { return sJobs.get(run.info.getId()) == run; } }

        private static void start(Run run) {
            android.net.Uri[] uris = null;
            String[] authorities = null;
            boolean content = run.content(), deadline = false;
            synchronized (sJobs) {
                run.planned = null;
                if (sJobs.get(run.info.getId()) != run || run.running) return;
                run.running = true;
                if (content) {
                    /* What was told changed is this run's; what is told from now on is the next one's. */
                    if (run.changedUris != null) uris = run.changedUris.toArray(new android.net.Uri[0]);
                    if (run.changedAuthorities != null) authorities = run.changedAuthorities.toArray(new String[0]);
                    run.changedUris = null; run.changedAuthorities = null;
                    deadline = !run.triggered && run.deadline != 0 && android.os.SystemClock.uptimeMillis() >= run.deadline;
                    run.triggered = false;
                    untime(run);
                }
            }
            try {
                android.app.Service s = megler.Services.service(run.info.getService());
                JobServiceEngine engine = null;
                if (s != null && !(s instanceof JobService))
                    engine = JobServiceEngine.of(s.onBind(new android.content.Intent("android.app.job.JobService").setComponent(run.info.getService())));
                if (!(s instanceof JobService) && engine == null) {
                    android.util.Log.w("Megler", "job " + run.info.getId() + ": " + run.info.getService().getClassName() + " is no job service of the app's");
                    forget(run);
                    return;
                }
                if (engine == null) run.service = (JobService) s;
                run.engine = engine;
                run.params = new JobParameters(run, uris, authorities, deadline);
                android.util.Log.i("Megler", "job " + run.info.getId() + " (" + run.info.getService().getShortClassName() + ") runs" + (engine != null ? " through its engine" : "")
                                   + (!content ? "" : (deadline ? " at its deadline," : "") + " for " + (uris != null ? uris.length : 0) + " change(s) of its content"));
                if (!(engine != null ? engine.onStartJob(run.params) : run.service.onStartJob(run.params))) finished(run.params, false);
            } catch (megler.Processes.Ended e) {
                // (its service's process ended as its Application was made, megler.Processes: the service is not made)
                android.util.Log.w("Megler", "job " + run.info.getId() + " not run: " + e.getMessage());
                forget(run);
            } catch (Throwable t) {
                android.util.Log.e("Megler", "job " + run.info.getId() + " failed", t);
                if (run.params != null) finished(run.params, false);
                else forget(run);
            }
        }
        /** A job that could not be started is the app's no more. */
        private static void forget(Run run) {
            synchronized (sJobs) { run.running = false; if (sJobs.get(run.info.getId()) == run) sJobs.remove(run.info.getId()); }
            unwatch(run);
        }

        /** The job of these parameters has ended (jobFinished, or no more work): again, or forgotten. */
        static void finished(JobParameters p, boolean reschedule) {
            Run run = p != null ? p.mRun : null;
            if (run == null) return;
            long again = -1;
            boolean gone = false;
            synchronized (sJobs) {
                if (!run.running) return;
                run.running = false;
                if (sJobs.get(run.info.getId()) != run) return;
                if (reschedule) {
                    run.failures++;
                    long b = Math.max(run.info.getInitialBackoffMillis(), 10000);
                    again = Math.min(JobInfo.MAX_BACKOFF_DELAY_MILLIS,
                                     run.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR ? b * run.failures : b << Math.min(run.failures - 1, 20));
                    /* A job of content triggers is told the changes of the run that failed again, before those told since. */
                    if (p.mTriggeredAuthorities != null) {
                        java.util.LinkedHashSet<android.net.Uri> uris = new java.util.LinkedHashSet<>();
                        java.util.LinkedHashSet<String> authorities = new java.util.LinkedHashSet<>(java.util.Arrays.asList(p.mTriggeredAuthorities));
                        if (p.mTriggeredUris != null) uris.addAll(java.util.Arrays.asList(p.mTriggeredUris));
                        if (run.changedUris != null) for (android.net.Uri u : run.changedUris) if (uris.size() < MAX_URIS_REPORTED) uris.add(u);
                        if (run.changedAuthorities != null) authorities.addAll(run.changedAuthorities);
                        run.changedUris = uris; run.changedAuthorities = authorities;
                    }
                } else if (!run.work.isEmpty()) {
                    again = 0;
                } else if (run.info.isPeriodic()) {
                    run.failures = 0;
                    again = run.info.getIntervalMillis();
                } else {
                    sJobs.remove(run.info.getId());
                    gone = true;
                }
                /* A job of content triggers that stays runs again for a change: one told meanwhile (or told the failed run), or the next. */
                if (again >= 0 && run.content()) {
                    run.earliest = android.os.SystemClock.uptimeMillis() + again;
                    run.deadline = 0;
                    run.triggered = run.changedAuthorities != null;
                    if (!run.triggered) again = -1;
                }
            }
            if (gone) unwatch(run);
            if (again >= 0) plan(run, again);
        }

        static JobWorkItem dequeue(JobParameters p) {
            Run run = p.mRun;
            JobWorkItem w;
            synchronized (sJobs) { w = run.running ? run.work.poll() : null; }
            if (w != null) { w.bumpDeliveryCount(); return w; }
            finished(p, false);
            return null;
        }

        private static void stop(Run run, int reason) {
            Runnable planned;
            boolean running;
            synchronized (sJobs) { planned = run.planned; run.planned = null; running = run.running; run.running = false; run.stopReason = reason; }
            if (planned != null) handler().removeCallbacks(planned);
            unwatch(run);
            if (running && (run.service != null || run.engine != null) && run.params != null) {
                final Run r = run;
                handler().post(() -> {
                    try { if (r.engine != null) r.engine.onStopJob(r.params); else r.service.onStopJob(r.params); }
                    catch (Throwable t) { android.util.Log.w("Megler", "job " + r.info.getId() + ": onStopJob failed", t); }
                });
            }
        }
    }
    /** One scheduler serves every namespace here. */
    public JobScheduler forNamespace(String namespace) { return this; }
    public String getNamespace() { return null; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static final int PENDING_JOB_REASON_APP = 1;
    public static final int PENDING_JOB_REASON_APP_STANDBY = 2;
    public static final int PENDING_JOB_REASON_BACKGROUND_RESTRICTION = 3;
    public static final int PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW = 4;
    public static final int PENDING_JOB_REASON_CONSTRAINT_CHARGING = 5;
    public static final int PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY = 6;
    public static final int PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER = 7;
    public static final int PENDING_JOB_REASON_CONSTRAINT_DEADLINE = 16;
    public static final int PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE = 8;
    public static final int PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY = 9;
    public static final int PENDING_JOB_REASON_CONSTRAINT_PREFETCH = 10;
    public static final int PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW = 11;
    public static final int PENDING_JOB_REASON_DEVICE_STATE = 12;
    public static final int PENDING_JOB_REASON_EXECUTING = -1;
    public static final int PENDING_JOB_REASON_INVALID_JOB_ID = -2;
    public static final int PENDING_JOB_REASON_JOB_SCHEDULER_OPTIMIZATION = 13;
    public static final int PENDING_JOB_REASON_QUOTA = 14;
    public static final int PENDING_JOB_REASON_UNDEFINED = 0;
    public static final int PENDING_JOB_REASON_USER = 15;
    public static final long THROW_ON_INVALID_DATA_TRANSFER_IMPLEMENTATION = 255371817L;
    public static java.lang.String sanitizeNamespace(java.lang.String p0) { return null; }
    public boolean canRunUserInitiatedJobs() { return false; }
    public void cancelInAllNamespaces() {}
    public java.util.List getAllJobSnapshots() { return new java.util.ArrayList(); }
    public int getPendingJobReason(int p0) { return 0; }
    public int[] getPendingJobReasons(int p0) { return null; }
    public java.util.List getPendingJobReasonsHistory(int p0) { return new java.util.ArrayList(); }
    public java.util.Map getPendingJobsInAllNamespaces() { return new java.util.HashMap(); }
    public java.util.List getStartedJobs() { return new java.util.ArrayList(); }
    public boolean hasRunUserInitiatedJobsPermission(java.lang.String p0, int p1) { return false; }
    public void notePendingUserRequestedAppStop(java.lang.String p0, int p1, java.lang.String p2) {}
    public int scheduleAsPackage(android.app.job.JobInfo p0, java.lang.String p1, int p2, java.lang.String p3) { return 0; }
    // ---- end of generated members
}
