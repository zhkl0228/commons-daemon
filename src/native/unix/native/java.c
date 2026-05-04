/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jsvc.h"

#ifdef OS_CYGWIN
typedef long long __int64;
#endif
#include <unistd.h>
#include <jni.h>

#ifdef CHARSET_EBCDIC
#ifdef OSD_POSIX
#include <ascii_ebcdic.h>
#define jsvc_xlate_to_ascii(b) _e2a(b)
#define jsvc_xlate_from_ascii(b) _a2e(b)
#endif
#else
#define jsvc_xlate_to_ascii(b)         /* NOOP */
#define jsvc_xlate_from_ascii(b)       /* NOOP */
#endif

static JavaVM *jvm = NULL;
static JNIEnv *env = NULL;
static jclass cls = NULL;

#define FALSE 0
#define TRUE !FALSE

static void shutdown(JNIEnv * env, jobject source, jboolean reload)
{
    log_debug("Shutdown requested (reload is %d)", reload);
    if (reload == TRUE)
        main_reload();
    else
        main_shutdown();
}

static void failed(JNIEnv * env, jobject source, jstring message)
{
    if (message) {
        const char *msg = (*env)->GetStringUTFChars(env, message, NULL);
        log_error("Failed %s", msg ? msg : "(null)");
        if (msg)
            (*env)->ReleaseStringUTFChars(env, message, msg);
    }
    else
        log_error("Failed requested");
    main_shutdown();
}

/* Automatically restart when the JVM crashes */
static void java_abort123(void)
{
    exit(123);
}

/*
 * Read the effective cgroup memory limit (in MB) for the current process.
 * Returns 0 when there is no effective limit (or the value cannot be read).
 *
 * Supports both cgroup v2 (/sys/fs/cgroup/memory.max) and cgroup v1
 * (/sys/fs/cgroup/memory/memory.limit_in_bytes). This makes autoHeapSize
 * safe to use inside Docker / Podman / Kubernetes / systemd slices on
 * CentOS / RHEL, where sysconf(_SC_PHYS_PAGES) reports the host's RAM
 * rather than the container's quota.
 */
static size_t read_cgroup_memory_limit_mb(void)
{
    static const char *paths[] = {
        "/sys/fs/cgroup/memory.max",                   /* cgroup v2 unified */
        "/sys/fs/cgroup/memory/memory.limit_in_bytes", /* cgroup v1 */
        NULL
    };
    int i;
    for (i = 0; paths[i] != NULL; i++) {
        FILE *fp = fopen(paths[i], "r");
        if (fp == NULL) {
            continue;
        }
        char buf[64];
        char *line = fgets(buf, sizeof(buf), fp);
        fclose(fp);
        if (line == NULL) {
            continue;
        }
        /* cgroup v2 reports the literal string "max" when there is no limit;
         * continue so that cgroup v1 is still checked on hybrid systems. */
        if (line[0] < '0' || line[0] > '9') {
            continue;
        }
        unsigned long long bytes = strtoull(line, NULL, 10);
        if (bytes == 0ULL) {
            return 0;
        }
        /* cgroup v1 represents "unlimited" as a huge sentinel value
         * (typically LONG_MAX rounded down to a page boundary, e.g.
         * 9223372036854771712). Anything >= 1 PiB is treated as "no limit". */
        if (bytes >= (1ULL << 50)) {
            return 0;
        }
        return (size_t)(bytes / (1024ULL * 1024ULL));
    }
    return 0;
}

/*
 * Compute a safe -Xmx (in MB) from the amount of RAM that is actually
 * available to this process. The strategy is tiered so that small VPS
 * boxes leave enough headroom for the JVM's non-heap footprint
 * (Metaspace, Code Cache, thread stacks, JNI / native libraries, and
 * Direct Memory) plus the operating system itself. Larger boxes can
 * dedicate a higher percentage to the heap.
 *
 *   <  2 GiB  -> auto-heap skipped; JVM ergonomics handle small machines
 *   <  4 GiB  -> 70%   (e.g.    2G ->  1433M heap, 615M reserved)
 *   <  8 GiB  -> 75%   (e.g.    4G ->  3072M heap, 1.0G reserved)
 *   < 16 GiB  -> 80%   (e.g.    8G ->  6553M heap, 1.6G reserved)
 *   >= 16 GiB -> 80%, but always keep at least 2 GiB reserved
 *                     (e.g.   16G -> 13107M heap, 3.2G reserved
 *                            32G -> 26214M heap, 6.4G reserved
 *                            64G -> 52428M heap, 12.8G reserved)
 *
 * NOTE: -Xmn is intentionally NOT set here. Modern collectors (G1, ZGC,
 * Shenandoah - the default since JDK 9 / 15) manage the young generation
 * adaptively and document that hard-coding -Xmn hurts both throughput
 * and pause times. Users who really want to tune the young generation
 * (e.g. when running ParallelGC) should still pass -Xmn explicitly.
 */
static size_t compute_auto_heap_mb(size_t availMB)
{
    size_t heapMB;
    if (availMB < 4096) {
        heapMB = availMB * 70 / 100;
    } else if (availMB < 8192) {
        heapMB = availMB * 75 / 100;
    } else if (availMB < 16384) {
        heapMB = availMB * 80 / 100;
    } else {
        heapMB = availMB * 80 / 100;
        /* Never claim the last 2 GiB, no matter how big the box is. */
        if (availMB > 2048 && heapMB > availMB - 2048) {
            heapMB = availMB - 2048;
        }
    }
    /* Sanity floor: the JVM itself needs a few dozen MB just to boot. */
    if (heapMB < 64) {
        heapMB = 64;
    }
    return heapMB;
}

char *java_library(arg_data *args, home_data *data)
{
    char *libf = NULL;

    /* Did we find ANY virtual machine? */
    if (data->jnum == 0) {
        log_error("Cannot find any VM in Java Home %s", data->path);
        return NULL;
    }

    /* Select the VM */
    if (args->name == NULL) {
        libf = data->jvms[0]->libr;
        log_debug("Using default JVM in %s", libf);
    }
    else {
        int x;
        for (x = 0; x < data->jnum; x++) {
            if (data->jvms[x]->name == NULL)
                continue;
            if (strcmp(args->name, data->jvms[x]->name) == 0) {
                libf = data->jvms[x]->libr;
                log_debug("Using specific JVM in %s", libf);
                break;
            }
        }
        if (libf == NULL) {
            log_error("Invalid JVM name specified %s", args->name);
            return NULL;
        }
    }
    return libf;
}

typedef jint(*jvm_create_t) (JavaVM **, JNIEnv **, JavaVMInitArgs *);

bool java_signal(void)
{
    jmethodID method;
    jboolean ret;
    char start[] = "signal";
    char startparams[] = "()Z";

    jsvc_xlate_to_ascii(start);
    jsvc_xlate_to_ascii(startparams);
    method = (*env)->GetStaticMethodID(env, cls, start, startparams);
    if (method == NULL) {
        (*env)->ExceptionClear(env);
        log_error("Cannot find DaemonLoader \"signal\" method");
        return false;
    }

    ret = (*env)->CallStaticBooleanMethod(env, cls, method);
    /* Clear any pending exception
     * so we can continue calling native methods
     */
    (*env)->ExceptionClear(env);
    log_debug("Daemon signal method returned %s", ret ? "true" : "false");
    return ret;
}

/* Initialize the JVM and its environment, loading libraries and all */
bool java_init(arg_data *args, home_data *data)
{
#ifdef OS_DARWIN
    dso_handle apph = NULL;
    char appf[1024];
    struct stat sb;
#endif /* ifdef OS_DARWIN */
    jvm_create_t symb = NULL;
    JNINativeMethod nativemethods[2];
    JavaVMOption *opt = NULL;
    dso_handle libh = NULL;
    JavaVMInitArgs arg;
    char *libf = NULL;
    jint ret;
    int x;
    char loaderclass[] = LOADER;
    char shutdownmethod[] = "shutdown";
    char shutdownparams[] = "(Z)V";
    char failedmethod[] = "failed";
    char failedparams[] = "(Ljava/lang/String;)V";
    char daemonprocid[64];
    /* Decide WHAT virtual machine we need to use */
    libf = java_library(args, data);
    if (libf == NULL) {
        log_error("Cannot locate JVM library file");
        return false;
    }

    /* Some Java options require environment variables to be set before loading
     * Java when using JNI
     */
    for (x = 0; x < args->onum; x++) {
        if (!strncmp(args->opts[x], "-XX:NativeMemoryTracking=", 25)) {
            fprintf(stdout, "Found [%s]\n", args->opts[x]);
            char *value = args->opts[x] + 25;
            fprintf(stdout, "Found value [%s]\n", value);
            if (strcmp(value, "off")) {
                snprintf(daemonprocid, sizeof(daemonprocid), "NMT_LEVEL_%d=%s", (int)getpid(), value);
                fprintf(stdout, "Setting environment entry [%s]\n", daemonprocid);
                putenv(daemonprocid);
            }
        }
    }

    /* Initialize the DSO library */
    if (dso_init() != true) {
        log_error("Cannot initialize the dynamic library loader");
        return false;
    }

    /* Load the JVM library */
#if !defined(OSD_POSIX)
    libh = dso_link(libf);
    if (libh == NULL) {
        log_error("Cannot dynamically link to %s", libf);
        log_error("%s", dso_error());
        return false;
    }
    log_debug("JVM library %s loaded", libf);
#endif

#ifdef OS_DARWIN
    /*
       MacOS/X actually has two libraries, one with the REAL vm, and one for
       the VM startup.
       - JVM 1.6, the library name is libverify.dylib
       - JVM 1.7 onwards, the library name is libjli.dylib
     */
    if (replace(appf, 1024, "$JAVA_HOME/../Libraries/libverify.dylib",
                "$JAVA_HOME", data->path) != 0) {
        log_error("Cannot replace values in loader library");
        return false;
    }
    if (stat(appf, &sb)) {
        if (replace(appf, 1024, "$JAVA_HOME/../MacOS/libjli.dylib", "$JAVA_HOME", data->path) != 0) {
            log_error("Cannot replace values in loader library");
            return false;
        }
    }
    // DAEMON-331 Alternative path for custom OpenJDK builds
    if (stat(appf, &sb)) {
        if (replace(appf, 1024, "$JAVA_HOME/lib/jli/libjli.dylib", "$JAVA_HOME", data->path) != 0) {
            log_error("Cannot replace values in loader library");
            return false;
        }
    }
    apph = dso_link(appf);
    if (apph == NULL) {
        log_error("Cannot load required shell library %s", appf);
        return false;
    }
    log_debug("Shell library %s loaded", appf);
#endif /* ifdef OS_DARWIN */
#if defined(OSD_POSIX)
    /* BS2000 does not allow to call JNI_CreateJavaVM indirectly */
#else
    symb = (jvm_create_t) dso_symbol(libh, "JNI_CreateJavaVM");
    if (symb == NULL) {
#ifdef OS_DARWIN
        symb = (jvm_create_t) dso_symbol(apph, "JNI_CreateJavaVM");
        if (symb == NULL) {
#endif /* ifdef OS_DARWIN */
            log_error("Cannot find JVM library entry point");
            return false;
#ifdef OS_DARWIN
        }
#endif /* ifdef OS_DARWIN */
    }
    log_debug("JVM library entry point found (0x%08X)", symb);
#endif

    /* Prepare the VM initialization arguments */

    /* Minimum Java version is Java 6 */
    arg.version = JNI_VERSION_1_6;

#if defined(OSD_POSIX)
    if (JNI_GetDefaultJavaVMInitArgs(&arg) < 0) {
        log_error("Cannot init default JVM default args");
        return false;
    }
#endif
    arg.ignoreUnrecognized = FALSE;
    bool autoHeapSize = TRUE;
    for (x = 0; x < args->onum; x++) {
        if(strstr(args->opts[x], "-Xms") ||
           strstr(args->opts[x], "-XX:InitialHeapSize") ||
           strstr(args->opts[x], "-Xmx") ||
           strstr(args->opts[x], "-XX:MaxHeapSize")) {
            autoHeapSize = FALSE;
            break;
        }
    }

    /* Decide the auto-heap size up front so that we can size the option
     * array correctly. A failure here (e.g. sysconf returning -1) simply
     * disables auto-heap and lets the JVM fall back to its built-in
     * ergonomics (which since JDK 8u131+ also honours cgroup limits). */
    size_t autoHeapMB = 0;
    if (autoHeapSize) {
        long pages    = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && pageSize > 0) {
            size_t physMB  = (size_t)pages * (size_t)pageSize / 1024 / 1024;
            size_t cgMB    = read_cgroup_memory_limit_mb();
            size_t availMB = physMB;
            if (cgMB > 0 && cgMB < availMB) {
                log_debug("autoHeapSize: cgroup memory limit %zuM is lower "
                          "than physical %zuM, using cgroup limit",
                          cgMB, physMB);
                availMB = cgMB;
            }
            if (availMB < 2048) {
                log_debug("autoHeapSize: available=%zuM < 2048M, skipping "
                          "heap options", availMB);
                autoHeapSize = FALSE;
            } else {
                autoHeapMB = compute_auto_heap_mb(availMB);
                log_debug("autoHeapSize: physical=%zuM available=%zuM "
                          "-> -XX:InitialHeapSize=%zuM -XX:MaxHeapSize=%zuM (%zu%%)",
                          physMB, availMB, autoHeapMB / 4,
                          autoHeapMB,
                          availMB > 0 ? (autoHeapMB * 100 / availMB) : 0);
            }
        } else {
            log_debug("autoHeapSize: sysconf(_SC_PHYS_PAGES/_SC_PAGESIZE) "
                      "returned an invalid value, falling back to JVM "
                      "default heap ergonomics");
            autoHeapSize = FALSE;
        }
    }

    arg.nOptions = args->onum + 5;     /* pid, ppid, version, class and abort */
    if (autoHeapSize) {
        arg.nOptions += 2;             /* InitialHeapSize, MaxHeapSize */
    }
    opt = (JavaVMOption *) malloc(arg.nOptions * sizeof(JavaVMOption));
    for (x = 0; x < args->onum; x++) {
        opt[x].optionString = strdup(args->opts[x]);
        jsvc_xlate_to_ascii(opt[x].optionString);
        opt[x].extraInfo = NULL;
    }
    if (autoHeapSize) {
        /* Start at 25 % of the max so the JVM commits memory on demand rather
         * than pre-allocating the full heap at startup. */
        size_t initHeapMB = autoHeapMB / 4;

        snprintf(daemonprocid, sizeof(daemonprocid),
                 "-XX:InitialHeapSize=%zuM", initHeapMB);
        opt[x].optionString = strdup(daemonprocid);
        jsvc_xlate_to_ascii(opt[x].optionString);
        opt[x++].extraInfo  = NULL;

        snprintf(daemonprocid, sizeof(daemonprocid),
                 "-XX:MaxHeapSize=%zuM", autoHeapMB);
        opt[x].optionString = strdup(daemonprocid);
        jsvc_xlate_to_ascii(opt[x].optionString);
        opt[x++].extraInfo  = NULL;
    }
    /* Add our daemon process id */
    snprintf(daemonprocid, sizeof(daemonprocid), "-Dcommons.daemon.process.id=%d", (int)getpid());
    opt[x].optionString = strdup(daemonprocid);
    jsvc_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo = NULL;

    snprintf(daemonprocid, sizeof(daemonprocid),
             "-Dcommons.daemon.process.parent=%d", (int)getppid());
    opt[x].optionString = strdup(daemonprocid);
    jsvc_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo = NULL;

    snprintf(daemonprocid, sizeof(daemonprocid),
             "-Dcommons.daemon.version=%s", JSVC_VERSION_STRING);
    opt[x].optionString = strdup(daemonprocid);
    jsvc_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo = NULL;

    /* DBCP-388. For the benefit of jconsole. */
    snprintf(daemonprocid, sizeof(daemonprocid), "-Dsun.java.command=%s", args->clas);
    opt[x].optionString = strdup(daemonprocid);
    jsvc_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo = NULL;

    opt[x].optionString = strdup("abort");
    jsvc_xlate_to_ascii(opt[x].optionString);
    opt[x].extraInfo = (void *)java_abort123;

    arg.options = opt;

    /* Do some debugging */
    if (log_debug_flag == true) {
        log_debug("+-- DUMPING JAVA VM CREATION ARGUMENTS -----------------");
        log_debug("| Version:                       %#08x", arg.version);
        log_debug("| Ignore Unrecognized Arguments: %s",
                  arg.ignoreUnrecognized == TRUE ? "True" : "False");
        log_debug("| Extra options:                 %d", args->onum);

        for (x = 0; x < args->onum; x++) {
            jsvc_xlate_from_ascii(opt[x].optionString);
            log_debug("|   \"%s\" (0x%08x)", opt[x].optionString, opt[x].extraInfo);
            jsvc_xlate_to_ascii(opt[x].optionString);
        }
        log_debug("+-------------------------------------------------------");
        log_debug("| Internal options:              %d", arg.nOptions - args->onum);

        for (; x < arg.nOptions; x++) {
            jsvc_xlate_from_ascii(opt[x].optionString);
            log_debug("|   \"%s\" (0x%08x)", opt[x].optionString, opt[x].extraInfo);
            jsvc_xlate_to_ascii(opt[x].optionString);
        }
        log_debug("+-------------------------------------------------------");
    }

    /* And finally create the Java VM */
#if defined(OSD_POSIX)
    ret = JNI_CreateJavaVM(&jvm, &env, &arg);
#else
    ret = (*symb) (&jvm, &env, &arg);
#endif
    if (ret < 0) {
        log_error("Cannot create Java VM");
        return false;
    }
    log_debug("Java VM created successfully");

    jsvc_xlate_to_ascii(loaderclass);
    cls = (*env)->FindClass(env, loaderclass);
    jsvc_xlate_from_ascii(loaderclass);
    if (cls == NULL) {
        log_error("Cannot find daemon loader %s", loaderclass);
        return false;
    }
    log_debug("Class %s found", loaderclass);

    jsvc_xlate_to_ascii(shutdownmethod);
    nativemethods[0].name = shutdownmethod;
    jsvc_xlate_to_ascii(shutdownparams);
    nativemethods[0].signature = shutdownparams;
    nativemethods[0].fnPtr = (void *)shutdown;
    jsvc_xlate_to_ascii(failedmethod);
    nativemethods[1].name = failedmethod;
    jsvc_xlate_to_ascii(failedparams);
    nativemethods[1].signature = failedparams;
    nativemethods[1].fnPtr = (void *)failed;

    if ((*env)->RegisterNatives(env, cls, nativemethods, 2) != 0) {
        log_error("Cannot register native methods");
        return false;
    }
    log_debug("Native methods registered");

    return true;
}

/* Destroy the Java VM */
bool JVM_destroy(int exit)
{
    jclass system = NULL;
    jmethodID method;
    char System[] = "java/lang/System";
    char exitclass[] = "exit";
    char exitparams[] = "(I)V";

    jsvc_xlate_to_ascii(System);
    system = (*env)->FindClass(env, System);
    jsvc_xlate_from_ascii(System);
    if (system == NULL) {
        log_error("Cannot find class %s", System);
        return false;
    }

    jsvc_xlate_to_ascii(exitclass);
    jsvc_xlate_to_ascii(exitparams);
    method = (*env)->GetStaticMethodID(env, system, exitclass, exitparams);
    if (method == NULL) {
        log_error("Cannot find \"System.exit(int)\" entry point");
        return false;
    }

    log_debug("Calling System.exit(%d)", exit);
    (*env)->CallStaticVoidMethod(env, system, method, (jint) exit);

    /* We shouldn't get here, but just in case... */
    log_debug("Destroying the Java VM");
    if ((*jvm)->DestroyJavaVM(jvm) != 0)
        return false;
    log_debug("Java VM destroyed");
    return true;
}

/* Call the load method in our DaemonLoader class */
bool java_load(arg_data *args)
{
    jclass stringClass = NULL;
    jstring className = NULL;
    jstring currentArgument = NULL;
    jobjectArray stringArray = NULL;
    jmethodID method = NULL;
    jboolean ret = FALSE;
    int x;
    char lang[] = "java/lang/String";
    char load[] = "load";
    char loadparams[] = "(Ljava/lang/String;[Ljava/lang/String;)Z";

    jsvc_xlate_to_ascii(args->clas);
    className = (*env)->NewStringUTF(env, args->clas);
    jsvc_xlate_from_ascii(args->clas);
    if (className == NULL) {
        log_error("Cannot create string for class name");
        return false;
    }

    jsvc_xlate_to_ascii(lang);
    stringClass = (*env)->FindClass(env, lang);
    if (stringClass == NULL) {
        log_error("Cannot find class java/lang/String");
        return false;
    }

    stringArray = (*env)->NewObjectArray(env, args->anum, stringClass, NULL);
    if (stringArray == NULL) {
        log_error("Cannot create arguments array");
        return false;
    }

    for (x = 0; x < args->anum; x++) {
        jsvc_xlate_to_ascii(args->args[x]);
        currentArgument = (*env)->NewStringUTF(env, args->args[x]);
        if (currentArgument == NULL) {
            jsvc_xlate_from_ascii(args->args[x]);
            log_error("Cannot create string for argument %s", args->args[x]);
            return false;
        }
        (*env)->SetObjectArrayElement(env, stringArray, x, currentArgument);
    }

    jsvc_xlate_to_ascii(load);
    jsvc_xlate_to_ascii(loadparams);
    method = (*env)->GetStaticMethodID(env, cls, load, loadparams);
    if (method == NULL) {
        log_error("Cannot find Daemon Loader \"load\" entry point");
        return false;
    }

    log_debug("Daemon loading...");
    ret = (*env)->CallStaticBooleanMethod(env, cls, method, className, stringArray);
    if (ret == FALSE) {
        log_error("Cannot load daemon");
        return false;
    }

    log_debug("Daemon loaded successfully");
    return true;
}

/* Call the start method in our daemon loader */
bool java_start(void)
{
    jmethodID method;
    jboolean ret;
    char start[] = "start";
    char startparams[] = "()Z";

    jsvc_xlate_to_ascii(start);
    jsvc_xlate_to_ascii(startparams);
    method = (*env)->GetStaticMethodID(env, cls, start, startparams);
    if (method == NULL) {
        log_error("Cannot find Daemon Loader \"start\" entry point");
        return false;
    }

    ret = (*env)->CallStaticBooleanMethod(env, cls, method);
    if (ret == FALSE) {
        log_error("Cannot start daemon");
        return false;
    }

    log_debug("Daemon started successfully");
    return true;
}

/*
 * call the java sleep to prevent problems with threads
 */
void java_sleep(int wait)
{
    jclass clsThread;
    jmethodID method;
    char jsleep[] = "sleep";
    char jsleepparams[] = "(J)V";
    char jthread[] = "java/lang/Thread";

    jsvc_xlate_to_ascii(jsleep);
    jsvc_xlate_to_ascii(jsleepparams);
    jsvc_xlate_to_ascii(jthread);

    clsThread = (*env)->FindClass(env, jthread);
    if (clsThread == NULL) {
        log_error("Cannot find java/lang/Thread class");
        return;
    }
    method = (*env)->GetStaticMethodID(env, clsThread, jsleep, jsleepparams);
    if (method == NULL) {
        log_error("Cannot found the sleep entry point");
        return;
    }

    (*env)->CallStaticVoidMethod(env, clsThread, method, (jlong) wait * 1000);
}

/* Call the stop method in our daemon loader */
bool java_stop(void)
{
    jmethodID method;
    jboolean ret;
    char stop[] = "stop";
    char stopparams[] = "()Z";

    jsvc_xlate_to_ascii(stop);
    jsvc_xlate_to_ascii(stopparams);
    method = (*env)->GetStaticMethodID(env, cls, stop, stopparams);
    if (method == NULL) {
        log_error("Cannot found Daemon Loader \"stop\" entry point");
        return false;
    }

    ret = (*env)->CallStaticBooleanMethod(env, cls, method);
    if (ret == FALSE) {
        log_error("Cannot stop daemon");
        return false;
    }

    log_debug("Daemon stopped successfully");
    return true;
}

/* Call the version method in our daemon loader */
bool java_version(void)
{
    jmethodID method;
    char version[] = "version";
    char versionparams[] = "()V";

    jsvc_xlate_to_ascii(version);
    jsvc_xlate_to_ascii(versionparams);
    method = (*env)->GetStaticMethodID(env, cls, version, versionparams);
    if (method == NULL) {
        log_error("Cannot found Daemon Loader \"version\" entry point");
        return false;
    }

    (*env)->CallStaticVoidMethod(env, cls, method);
    return true;
}

/* Call the check method in our DaemonLoader class */
bool java_check(arg_data *args)
{
    jstring className = NULL;
    jmethodID method = NULL;
    jboolean ret = FALSE;
    char check[] = "check";
    char checkparams[] = "(Ljava/lang/String;)Z";

    log_debug("Checking daemon");

    jsvc_xlate_to_ascii(args->clas);
    className = (*env)->NewStringUTF(env, args->clas);
    jsvc_xlate_from_ascii(args->clas);
    if (className == NULL) {
        log_error("Cannot create string for class name");
        return false;
    }

    jsvc_xlate_to_ascii(check);
    jsvc_xlate_to_ascii(checkparams);
    method = (*env)->GetStaticMethodID(env, cls, check, checkparams);
    if (method == NULL) {
        log_error("Cannot found Daemon Loader \"check\" entry point");
        return false;
    }

    ret = (*env)->CallStaticBooleanMethod(env, cls, method, className);
    if (ret == FALSE) {
        log_error("An error was detected checking the %s daemon", args->clas);
        return false;
    }

    log_debug("Daemon checked successfully");
    return true;
}

/* Call the destroy method in our daemon loader */
bool java_destroy(void)
{
    jmethodID method;
    jboolean ret;
    char destroy[] = "destroy";
    char destroyparams[] = "()Z";

    jsvc_xlate_to_ascii(destroy);
    jsvc_xlate_to_ascii(destroyparams);
    method = (*env)->GetStaticMethodID(env, cls, destroy, destroyparams);
    if (method == NULL) {
        log_error("Cannot found Daemon Loader \"destroy\" entry point");
        return false;
    }

    ret = (*env)->CallStaticBooleanMethod(env, cls, method);
    if (ret == FALSE) {
        log_error("Cannot destroy daemon");
        return false;
    }

    log_debug("Daemon destroyed successfully");
    return true;
}
