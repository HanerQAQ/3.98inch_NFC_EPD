package org.nfcresearch.qpolllab;

import android.nfc.Tag;
import android.nfc.tech.NfcA;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemClock;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.channels.FileLock;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

/** Bounded root helper for observation, identity reads, and explicitly requested image refresh. */
public final class QPollWorker {
    private static Object service, callback;
    private static Class<?> adapterType;
    private static final Binder token = new Binder();
    private static final AtomicBoolean stopping = new AtomicBoolean();
    private static final AtomicBoolean cleaned = new AtomicBoolean();
    private static final AtomicBoolean tagSeen = new AtomicBoolean();
    private static volatile boolean readerTouched, qtagTouched, readerReleased, qtagDisabled, xiaomiQTagTouched, vivoTechTouched;
    private static int discoveredCode, lostCode = -1;
    private static Process logcat;
    private static volatile Tag detectedTag;
    private static volatile String qtagRejection;
    private static volatile boolean qDiscoverySent,qDiscoveryAccepted;
    private static String mode;
    private static final Binder callbackBinder = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == IBinder.INTERFACE_TRANSACTION) {
                if (reply != null) reply.writeString("android.nfc.IAppCallback");
                return true;
            }
            if (code != discoveredCode && code != lostCode) return super.onTransact(code, data, reply, flags);
            data.enforceInterface("android.nfc.IAppCallback");
            Tag tag = data.readInt() == 0 ? null : Tag.CREATOR.createFromParcel(data);
            if (code == discoveredCode && tag != null && tagSeen.compareAndSet(false,true)) {
                detectedTag=tag;
                emit("TAG", "收到标签 UID=" + hex(tag.getId()) + "；技术=" + Arrays.toString(tag.getTechList()));
                NfcA a = NfcA.get(tag);
                if (a != null) emit("META", "ATQA=" + hex(a.getAtqa()) + "；SAK=" + String.format("%04X", a.getSak() & 0xffff));
                stopping.set(true);
            } else if (code == lostCode) {
                emit("LOST", "收到标签离场通知（不代表已读到目标屏）");
            }
            if (reply != null) reply.writeNoException();
            return true;
        }
    };

    private static synchronized void emit(String kind, String message) {
        System.out.println("QP|" + kind + "|" + message.replace('\n', ' '));
        System.out.flush();
    }
    private static String hex(byte[] bytes) {
        if (bytes == null) return "null";
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format("%02X", b & 255));
        return s.toString();
    }
    private static Object invoke(String name, Class<?>[] types, Object... args) throws Exception {
        try { return adapterType.getMethod(name, types).invoke(service, args); }
        catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            throw e;
        }
    }
    private static void reader(int flags) throws Exception {
        Bundle extras = new Bundle();
        if ("refresh".equals(mode) || "identity".equals(mode)) extras.putInt("presence", 5000);
        extras.putBoolean("qreader", true);
        extras.putInt("pollTech", 0x101);
        invoke("setReaderMode", new Class<?>[]{IBinder.class, Class.forName("android.nfc.IAppCallback"),
                int.class, Bundle.class, String.class}, token, callback, flags, extras, android.os.Process.myUid() == 2000 ? "com.android.shell" : "android");
    }
    private static int qtag(int action) throws Exception {
        return (Integer) invoke("sendVendorNciMessage", new Class<?>[]{int.class,int.class,int.class,byte[].class},
                1, 15, 112, new byte[]{0x31, (byte) action});
    }
    private static boolean changeVivoDiscoveryTech(int pollTech, int listenTech) {
        try {
            Method vm = adapterType.getMethod("getNfcAdapterVendorInterface", String.class);
            IBinder b = (IBinder) vm.invoke(service, "nxp");
            if (b != null) {
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                data.writeInterfaceToken("com.nxp.nfc.INxpNfcAdapter");
                data.writeStrongBinder(token);
                data.writeInt(pollTech);
                data.writeInt(listenTech);
                boolean res = b.transact(10, data, reply, 0);
                reply.readException();
                data.recycle();
                reply.recycle();
                return res;
            }
        } catch (Throwable t) {
            emit("VENDOR_FALLBACK", "Vivo changeDiscoveryTech 未生效：" + t.getMessage());
        }
        return false;
    }
    private static void init() throws Exception {
        if (android.os.Process.myUid() != 0 && android.os.Process.myUid()!=2000) throw new SecurityException("需要 Shizuku 或 Root 授权");
        Object binder = Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "nfc");
        if (binder == null) throw new IllegalStateException("未找到 NFC 服务");
        adapterType = Class.forName("android.nfc.INfcAdapter");
        service = Class.forName("android.nfc.INfcAdapter$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
        int state = (Integer) invoke("getState", new Class<?>[0]);
        emit("STATE", "执行身份 UID="+android.os.Process.myUid()+"；NFC 状态=" + state + (state == 3 ? "（已开启）" : "（未开启）"));
        if (state != 3) throw new IllegalStateException("请先在系统设置中开启 NFC");
    }
    private static void setupCallback() throws Exception {
        Class<?> type = Class.forName("android.nfc.IAppCallback");
        Class<?> stub = Class.forName("android.nfc.IAppCallback$Stub");
        Field field = stub.getDeclaredField("TRANSACTION_onTagDiscovered");
        field.setAccessible(true); discoveredCode = field.getInt(null);
        try { field = stub.getDeclaredField("TRANSACTION_onTagLost"); field.setAccessible(true); lostCode = field.getInt(null); }
        catch (NoSuchFieldException ignored) { }
        callback = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type}, (object, method, args) -> {
            if (method.getName().equals("asBinder")) return callbackBinder;
            if (method.getName().equals("toString")) return "QPollLabCallback";
            if (method.getName().equals("hashCode")) return System.identityHashCode(object);
            if (method.getName().equals("equals")) return object == args[0];
            return null;
        });
    }
    private static void startLogcat() {
        try {
            String since=new java.text.SimpleDateFormat("MM-dd HH:mm:ss.SSS",java.util.Locale.US).format(new java.util.Date());
            logcat = new ProcessBuilder("/system/bin/logcat", "-v", "threadtime", "-T", since,
                    "NxpNciX:I", "NxpNciR:I", "*:S").redirectErrorStream(true).start();
            Thread monitor = new Thread(() -> {
                try (BufferedReader in = new BufferedReader(new InputStreamReader(logcat.getInputStream()))) {
                    String line;
                    while ((line = in.readLine()) != null) {
                        int marker = line.indexOf("> ");
                        if (marker < 0) continue;
                        String packet = line.substring(marker + 2).trim();
                        if(!cleaned.get()&&line.contains("NxpNciX")&&NciDiscovery.hasQPoll(packet)){qDiscoverySent=true;emit("CAPABILITY","已发出 0x71 Q-Poll 寻卡，等待控制器确认。");}
                        if(!cleaned.get()&&qDiscoverySent&&line.contains("NxpNciR")&&packet.startsWith("410301")&&packet.length()>=8){
                            if(packet.substring(6,8).equals("00")){qDiscoveryAccepted=true;emit("CAPABILITY","已确认控制器接受 Q-Poll 寻卡（0x71）；目标屏通信与刷新需单独验证。");}
                            else emit("CAPABILITY","Q-Poll 寻卡被拒绝，响应状态="+packet.substring(6,8));
                        }
                        if(line.contains("NxpNciR")&&packet.toUpperCase(java.util.Locale.ROOT).startsWith("4F7001")&&packet.length()>=8&&!cleaned.get()){
                            String status=packet.substring(6,8);
                            if(!status.equals("00")){qtagRejection="系统拒绝 QTag，响应状态="+status+"；当前系统未成功开启 Q-Poll。";stopping.set(true);emit("QTAG_REJECTED",qtagRejection);}
                        }
                        if (line.contains("NxpNciX") && packet.startsWith("2103")) emit("RF_TX", packet);
                        if (line.contains("NxpNciR") && (packet.startsWith("4103") || packet.startsWith("6103") || packet.startsWith("6105"))) emit("RF_RX", packet);
                    }
                } catch (Exception ignored) { }
            }, "nci-monitor");
            monitor.setDaemon(true); monitor.start();
        } catch (Exception e) { emit("LOG_UNAVAILABLE", "无法读取 HAL 日志；仍可观察标签回调：" + e); }
    }
    private static synchronized void cleanup() {
        if (!cleaned.compareAndSet(false, true)) return;
        if (readerTouched) {
            try { reader(0); readerReleased = true; emit("READER_OFF", "ReaderMode 已释放"); }
            catch (Exception e) { emit("CLEANUP_ERROR", "释放 ReaderMode 失败：" + e); }
        }
        if (qtagTouched) {
            try {
                int status = qtag(0);
                qtagDisabled = status == 0;
                emit(status == 0 ? "QTAG_OFF" : "CLEANUP_ERROR", "关闭 QTag，API 状态=" + status);
            } catch (Exception e) { emit("CLEANUP_ERROR", "关闭 QTag 失败：" + e); }
        }
        if (vivoTechTouched) {
            try {
                changeVivoDiscoveryTech(0xFF, 0xC4);
                emit("VENDOR_OFF", "Vivo/NXP 厂商寻卡配置已复原");
            } catch (Exception e) { emit("CLEANUP_ERROR", "复原 NXP 厂商接口失败：" + e); }
        }
        SystemClock.sleep(150);
        if (logcat != null) logcat.destroy();
    }
    public static void main(String[] args) {
        int exit = 0;
        boolean sessionStarted = false;
        try {
            if ((args.length != 4&&args.length!=5) || !(args[0].equals("inspect") || args[0].equals("qtag") || args[0].equals("standard")||args[0].equals("identity")||args[0].equals("refresh")))
                throw new IllegalArgumentException("Invalid mode or argument count");
            mode=args[0];
            boolean panelOperation=mode.equals("identity")||mode.equals("refresh");
            if(panelOperation&&args.length!=5)throw new IllegalArgumentException("Missing private image job");
            int seconds = Integer.parseInt(args[1]);
            if (seconds < 1 || seconds > 60) throw new IllegalArgumentException("时长超出当前模式允许范围");
            File stopFile = new File(args[2]);
            if (stopFile.exists()) { emit("END", "已取消，未启动寻卡"); return; }
            // App creates this private file before invoking root; no global writable lock path.
            try (RandomAccessFile lockFile = new RandomAccessFile(args[3], "rw"); FileLock lock = lockFile.getChannel().tryLock()) {
                if (lock == null) throw new IllegalStateException("另一轮测试正在运行");
                try {
                init();
                if (args[0].equals("inspect")) {
                    emit("CHIP", ChipInfo.inspect());
                    emit("METHODS", "--- INfcAdapter methods ---");
                    for (Method m : adapterType.getMethods()) {
                        String name = m.getName();
                        if (name.contains("Nxp") || name.contains("Vendor") || name.contains("Reader") || name.contains("Poll") || name.contains("Tech") || name.contains("Vivo")) {
                            StringBuilder sb = new StringBuilder(m.getName()).append("(");
                            for (Class<?> p : m.getParameterTypes()) sb.append(p.getSimpleName()).append(",");
                            if (m.getParameterTypes().length > 0) sb.deleteCharAt(sb.length() - 1);
                            sb.append("): ").append(m.getReturnType().getSimpleName());
                            emit("METHOD", sb.toString());
                        }
                    }
                    // Try getNfcAdapterVendorInterface for nxp and vivo
                    for (String vendor : new String[]{"nxp", "vivo", "extra", "wlc"}) {
                        try {
                            Method vm = adapterType.getMethod("getNfcAdapterVendorInterface", String.class);
                            IBinder b = (IBinder) vm.invoke(service, vendor);
                            if (b != null) {
                                emit("VENDOR_IFACE", vendor + " -> descriptor: " + b.getInterfaceDescriptor() + " (" + b.getClass().getName() + ")");
                                // Try asInterface if class exists
                                String desc = b.getInterfaceDescriptor();
                                if ("nxp".equals(vendor)) {
                                    try {
                                        Parcel data = Parcel.obtain();
                                        Parcel reply = Parcel.obtain();
                                        data.writeInterfaceToken("com.nxp.nfc.INxpNfcAdapter");
                                        data.writeStrongBinder(token);
                                        data.writeInt(0x101); // pollTech: NFC-A + 0x100
                                        data.writeInt(0);     // listenTech: 0
                                        boolean res = b.transact(10, data, reply, 0);
                                        reply.readException();
                                        emit("CHANGE_TECH", "changeDiscoveryTech(0x101, 0) returned: " + res);
                                        data.recycle();
                                        reply.recycle();
                                    } catch (Throwable t) {
                                        emit("CHANGE_TECH_ERR", "changeDiscoveryTech error: " + t);
                                    }
                                }
                            } else {
                                emit("VENDOR_IFACE", vendor + " -> null");
                            }
                        } catch (Throwable t) {
                            emit("VENDOR_ERR", vendor + ": " + t);
                        }
                    }
                    try {
                        adapterType.getMethod("sendVendorNciMessage", int.class, int.class, int.class, byte[].class);
                        emit("READY", "NXP 测试入口存在，可开始测试（尚未验证空口能力）");
                    } catch (NoSuchMethodException e) {
                        emit("NO_VENDOR_NCI", "INfcAdapter 无 sendVendorNciMessage 接口");
                    }
                    emit("END", "权限检查完成，未发送寻卡命令"); return;
                }
                if (stopFile.exists()) { emit("END", "已取消，未启动寻卡"); return; }
                setupCallback();
                Runtime.getRuntime().addShutdownHook(new Thread(QPollWorker::cleanup));
                startLogcat();
                if (args[0].equals("qtag") || panelOperation) {
                    // Tier 1: Try Xiaomi NXP Vendor NCI
                    try {
                        int status = qtag(1);
                        if (status == 0) {
                            xiaomiQTagTouched = true;
                            qtagTouched = true;
                            emit("QTAG_ON", "Tier 1: 启用 Xiaomi QTag 成功（状态=0）");
                        } else {
                            emit("QTAG_FALLBACK", "Tier 1: Xiaomi Vendor NCI 响应状态=" + status + "，尝试 Tier 2 厂商接口");
                        }
                    } catch (Throwable t) {
                        emit("QTAG_FALLBACK", "Tier 1: 未找到 Xiaomi Vendor NCI 接口，尝试 Tier 2 厂商接口");
                    }
                }

                readerTouched = true;
                // Tier 3: ReaderMode with full extended flag matrix (0x81 | 0x20 | 0x100)
                int readerFlags = 0x81;
                if (args[0].equals("qtag") || panelOperation) {
                    readerFlags |= 0x20;  // FLAG_READER_NFC_Q (NXP custom bit 5)
                    readerFlags |= 0x100; // FLAG_READER_NO_PLATFORM_SOUNDS / Huawei QReader trigger
                }
                reader(readerFlags);

                // Tier 2: Try Vivo / NXP Vendor Interface changeDiscoveryTech AFTER ReaderMode is established
                if ((args[0].equals("qtag") || panelOperation) && !xiaomiQTagTouched) {
                    if (changeVivoDiscoveryTech(0x121, 0)) {
                        vivoTechTouched = true;
                        emit("CAPABILITY", "Tier 2: 已在 ReaderMode 激活后通过 NXP 厂商接口注入 0x121 (A+Q+QPoll) 扩展寻卡");
                    } else if (changeVivoDiscoveryTech(0x101, 0)) {
                        vivoTechTouched = true;
                        emit("CAPABILITY", "Tier 2: 已在 ReaderMode 激活后通过 NXP 厂商接口激活 0x101 扩展寻卡");
                    }
                }
                sessionStarted = true;
                emit("START", args[0] + ":" + seconds);
                long deadline = SystemClock.elapsedRealtime() + seconds * 1000L;
                while (!stopping.get() && !stopFile.exists() && SystemClock.elapsedRealtime() < deadline) SystemClock.sleep(100);
                if (qtagRejection != null && xiaomiQTagTouched) throw new IllegalStateException(qtagRejection);
                if ((mode.equals("qtag") || panelOperation) && !qDiscoveryAccepted) emit("CAPABILITY", "尚未确认 Q-Poll：未获得 0x71 指令及成功响应的完整证据；请查看日志。");
                if (tagSeen.get()&&panelOperation) {
                    final int scanSeconds=seconds;
                    final File panelJob=new File(args[4]);
                    final byte[] targetUid=detectedTag.getId().clone();
                    final int[] connectionAttempt={0};
                    ConnectionRetry.run(()->{
                        if(stopFile.exists())throw new InterruptedException("已停止，未开始协议通信");
                        if(!Arrays.equals(targetUid,detectedTag.getId()))throw new java.io.IOException("重新寻到不同 UID 的标签，停止；尚未发送 APDU");
                        emit("CONNECT_ATTEMPT",(++connectionAttempt[0])+" / 3");
                        PanelSession.run(detectedTag,mode,panelJob,stopFile,QPollWorker::emit);
                    },()->{
                        if(stopFile.exists())throw new InterruptedException("已停止重新寻卡");
                        emit("RECONNECT","尚未发送 APDU；重新寻卡以获取新标签，最多三次连接尝试");
                        reader(0);
                        SystemClock.sleep(200);
                        detectedTag=null;tagSeen.set(false);stopping.set(false);
                        if(stopFile.exists())throw new InterruptedException("已停止重新寻卡");
                        reader(0x81);
                        emit("START",mode+":"+scanSeconds);
                        long retryDeadline=SystemClock.elapsedRealtime()+scanSeconds*1000L;
                        while(!stopping.get()&&!stopFile.exists()&&SystemClock.elapsedRealtime()<retryDeadline)SystemClock.sleep(20);
                        if(stopFile.exists())throw new InterruptedException("已停止重新寻卡");
                        if(qtagRejection!=null)throw new IllegalStateException(qtagRejection);
                        if(!tagSeen.get())throw new java.io.IOException("连接失败后重新寻卡超时；尚未发送 APDU");
                    });
                    if(mode.equals("identity"))emit("RESULT","D1 屏幕规格与原厂配置校验通过，未写图像");
                }
                else if (tagSeen.get()) emit("RESULT", "已收到标签；只显示元信息，未发送 APDU");
                else if (stopFile.exists()) emit("RESULT", "用户停止；未收到标签");
                else emit("RESULT", "本轮超时，未收到标签；不能据此认定硬件不支持");
                } finally { cleanup(); }
            }
        } catch (InterruptedException e) {
            emit("CANCELLED",e.getMessage());exit=3;
        } catch (Throwable e) {
            emit("ERROR", e.getClass().getSimpleName() + ": " + e.getMessage()); exit = 1;
            java.io.StringWriter trace=new java.io.StringWriter();e.printStackTrace(new java.io.PrintWriter(trace));
            for(String line:trace.toString().split("\n"))emit("TRACE",line.trim());
        } finally {
            cleanup();
            if (readerTouched || qtagTouched) {
                boolean ok = (!readerTouched || readerReleased) && (!qtagTouched || qtagDisabled);
                emit(ok ? "RESTORED" : "RESTORE_FAILED", ok ? "已恢复普通寻卡" : "恢复未确认，请重启 NFC 开关后再试");
                if (!ok) exit = 2;
            }
            if (sessionStarted || exit != 0) emit("END", "本轮结束");
        }
        System.exit(exit);
    }
}
