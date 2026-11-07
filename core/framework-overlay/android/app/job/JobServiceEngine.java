// Overlay of core/framework/gen-src/android/app/job/JobServiceEngine.java (tools/build-framework.sh).
// It differs in being a job's engine Megler's job scheduler runs (JobScheduler, overlaid): getBinder gives a binder of this
// engine, which the scheduler finds in what the service's onBind returns and starts and stops the job through; jobFinished
// tells the scheduler the job has ended, as JobService's does. The generated stub kept nothing: AndroidX's JobIntentService
// (on API 26 and later a Service whose onBind returns its engine's binder, not a JobService) was never run, and TikTok Lite's
// FcmRegistrationJobIntentService (package 609) was dropped with "is no job service of the app's".
package android.app.job;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public abstract class JobServiceEngine {
    /** What getBinder gives: the engine, for the scheduler of this process to find. */
    static final class EngineBinder extends android.os.Binder {
        final JobServiceEngine engine;
        EngineBinder(JobServiceEngine e) { engine = e; }
    }
    private final android.app.Service mService;
    private final EngineBinder mBinder = new EngineBinder(this);
    public JobServiceEngine(android.app.Service service) { mService = service; }
    JobServiceEngine() { this((android.app.Service) null); }
    public final android.os.IBinder getBinder() { return mBinder; }
    /** The engine of a service's binder, or null when it is none. */
    static JobServiceEngine of(android.os.IBinder b) { return b instanceof EngineBinder ? ((EngineBinder) b).engine : null; }
    public abstract boolean onStartJob(JobParameters params);
    public abstract boolean onStopJob(JobParameters params);
    public void jobFinished(JobParameters params, boolean needsReschedule) { JobScheduler.Impl.finished(params, needsReschedule); }
    public long getTransferredDownloadBytes(JobParameters p0, JobWorkItem p1) { return 0L; }
    public long getTransferredUploadBytes(JobParameters p0, JobWorkItem p1) { return 0L; }
    public void onNetworkChanged(JobParameters p0) {}
    public void setNotification(JobParameters p0, int p1, android.app.Notification p2, int p3) {}
    public void updateEstimatedNetworkBytes(JobParameters p0, JobWorkItem p1, long p2, long p3) {}
    public void updateTransferredNetworkBytes(JobParameters p0, JobWorkItem p1, long p2, long p3) {}
}
