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
// Not here: jobs persisted across the app's runs (setPersisted), constraints other than the time (charging, idle, a network
// of a kind: taken to hold), content triggers, and the deadline: a job runs once it may.
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
        JobParameters params;
        Runnable planned;
        boolean running;
        int failures, stopReason;
        Run(JobInfo info) { this.info = info; }
    }

    /**
     * The app's jobs, every instance of the scheduler the same ones (each getSystemService makes one), run as Android's job
     * scheduler runs them: on the main thread, once the minimum latency has passed (a network asked for is taken to be there),
     * by onStartJob of the job's service; the job ends when onStartJob answers false, at jobFinished, or when dequeueWork has no
     * more; then again after its backoff if it asked to be rescheduled, after its interval if it repeats, at once if work was
     * enqueued meanwhile. Scheduling a job again, or cancelling it, stops it if it runs (onStopJob).
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
            synchronized (sJobs) { old = sJobs.put(j.getId(), run); }
            if (old != null) stop(old, 1 /* STOP_REASON_CANCELLED_BY_APP */);
            plan(run, j.getMinLatencyMillis());
            return RESULT_SUCCESS;
        }
        public int enqueue(JobInfo j, JobWorkItem w) {
            if (j == null || j.getService() == null) return RESULT_FAILURE;
            Run run;
            synchronized (sJobs) {
                run = sJobs.get(j.getId());
                if (run != null && j.getService().equals(run.info.getService())) {
                    if (w != null) run.work.add(w);
                    if (run.running || run.planned != null) return RESULT_SUCCESS;
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
            synchronized (sJobs) { run.planned = null; if (sJobs.get(run.info.getId()) != run || run.running) return; run.running = true; }
            try {
                android.app.Service s = megler.Services.service(run.info.getService());
                if (!(s instanceof JobService)) {
                    android.util.Log.w("Megler", "job " + run.info.getId() + ": " + run.info.getService().getClassName() + " is no job service of the app's");
                    synchronized (sJobs) { run.running = false; if (sJobs.get(run.info.getId()) == run) sJobs.remove(run.info.getId()); }
                    return;
                }
                run.service = (JobService) s;
                run.params = new JobParameters(run);
                android.util.Log.i("Megler", "job " + run.info.getId() + " (" + run.info.getService().getShortClassName() + ") runs");
                if (!run.service.onStartJob(run.params)) finished(run.params, false);
            } catch (Throwable t) {
                android.util.Log.e("Megler", "job " + run.info.getId() + " failed", t);
                if (run.params != null) finished(run.params, false);
                else synchronized (sJobs) { run.running = false; if (sJobs.get(run.info.getId()) == run) sJobs.remove(run.info.getId()); }
            }
        }

        /** The job of these parameters has ended (jobFinished, or no more work): again, or forgotten. */
        static void finished(JobParameters p, boolean reschedule) {
            Run run = p != null ? p.mRun : null;
            if (run == null) return;
            long again = -1;
            synchronized (sJobs) {
                if (!run.running) return;
                run.running = false;
                if (sJobs.get(run.info.getId()) != run) return;
                if (reschedule) {
                    run.failures++;
                    long b = Math.max(run.info.getInitialBackoffMillis(), 10000);
                    again = Math.min(JobInfo.MAX_BACKOFF_DELAY_MILLIS,
                                     run.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR ? b * run.failures : b << Math.min(run.failures - 1, 20));
                } else if (!run.work.isEmpty()) {
                    again = 0;
                } else if (run.info.isPeriodic()) {
                    run.failures = 0;
                    again = run.info.getIntervalMillis();
                } else {
                    sJobs.remove(run.info.getId());
                }
            }
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
            if (running && run.service != null && run.params != null) {
                final Run r = run;
                handler().post(() -> { try { r.service.onStopJob(r.params); } catch (Throwable t) { android.util.Log.w("Megler", "job " + r.info.getId() + ": onStopJob failed", t); } });
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
