/*
 *
 * NEC SX-Aurora TSUBASA Vector Engine (VE) support for nvtop.
 *
 * Data sources (all read-only; nothing on the VE or the host is changed):
 *   - /sys/class/ve/veN      : enumeration of VE devices (N = VE node number)
 *   - vecmd info             : temperature, fan, 12V voltage/current (power), clocks, memory size, PCIe link
 *   - ve-mpstat 1            : utilisation of one VE node. The "all" row is the average over all cores
 *                              (100 - %idle of the whole device; one busy core of eight = 12.5 %).
 *   - ve-top -b -d 1         : memory usage ("MiB Mem ... used") and the process table
 *                              (PID, USER, %CPU, RES, COMMAND). The PID is the host ve_exec PID.
 *                              %CPU follows ve-top: one fully used core is 100.
 *
 * ve-mpstat and ve-top ignore -N for VE1 in this environment; the VE node is selected with the
 * environment variable VE_NODE_NUMBER=N, so one long-running child per (VE, tool) is started.
 *
 * The number shown for a VE is its node number (VE0, VE1), taken from /sys/class/ve, not nvtop's
 * position in the device list.
 *
 * This file is part of Nvtop and is distributed under the GNU GPL v3 or later.
 *
 */

#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/time.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define VE_SYSFS_CLASS_DIR "/sys/class/ve"
#define VE_VECMD_PATH "/opt/nec/ve/bin/vecmd"
#define VE_MPSTAT_PATH "/opt/nec/ve/bin/ve-mpstat"
#define VE_TOP_PATH "/opt/nec/ve/bin/ve-top"
#define VE_MAX_NODES 16
#define VE_VECMD_CACHE_NS UINT64_C(500000000)   // vecmd is refreshed at most every 0.5 s
#define VE_SAMPLE_MAX_AGE_NS UINT64_C(3000000000) // utilisation / memory older than 3 s is not reported
#define VE_STREAM_BUF_LEN 4096
#define VE_STREAM_MAX_TOKENS 32
#define VE_TOP_PROCS 64
// Rated fan speed of the VE (ve_hw_spec.yaml, ve_fan max_value). The VE only reports RPM,
// so the percentage shown for it is RPM / rated RPM, not the PWM duty of the fan controller.
#define VE_FAN_MAX_RPM 10600u

struct gpu_info_ve {
  struct gpu_info base;
  unsigned node;
};

struct ve_node_sample {
  bool temp_valid;
  bool fan_valid;
  bool core_clock_valid;
  bool mem_clock_valid;
  bool mem_size_valid;
  bool pcie_gen_valid;
  bool pcie_width_valid;
  bool power_valid;
  bool type_valid;
  char type[16]; // VE type from "VE Type", e.g. "10C"
  unsigned temp_max; // degC, maximum over the "Chip Core" sensors
  unsigned fan_rpm;
  unsigned core_clock_mhz;
  unsigned mem_clock_mhz;
  unsigned long long mem_total; // bytes
  unsigned pcie_gen;
  unsigned pcie_width;
  unsigned rail_mask; // which of the 12V rail values were parsed
  double aux_v, aux_c;   // AUX 12V rail: V and mA
  double edge_v, edge_c; // Edge 12V rail: V and mA
};

#define VE_RAIL_AUX_V (1u << 0)
#define VE_RAIL_AUX_C (1u << 1)
#define VE_RAIL_EDGE_V (1u << 2)
#define VE_RAIL_EDGE_C (1u << 3)
#define VE_RAIL_ALL (VE_RAIL_AUX_V | VE_RAIL_AUX_C | VE_RAIL_EDGE_V | VE_RAIL_EDGE_C)

static bool gpuinfo_ve_init(void);
static void gpuinfo_ve_shutdown(void);
static const char *gpuinfo_ve_last_error_string(void);
static bool gpuinfo_ve_get_device_handles(struct list_head *devices, unsigned *count);
static void gpuinfo_ve_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_ve_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_ve_refresh_utilisation_rate(struct gpu_info *_gpu_info);
static void gpuinfo_ve_refresh_running_processes(struct gpu_info *_gpu_info);

struct gpu_vendor gpu_vendor_ve = {
    .init = gpuinfo_ve_init,
    .shutdown = gpuinfo_ve_shutdown,
    .last_error_string = gpuinfo_ve_last_error_string,
    .get_device_handles = gpuinfo_ve_get_device_handles,
    .populate_static_info = gpuinfo_ve_populate_static_info,
    .refresh_dynamic_info = gpuinfo_ve_refresh_dynamic_info,
    .refresh_utilisation_rate = gpuinfo_ve_refresh_utilisation_rate,
    .refresh_running_processes = gpuinfo_ve_refresh_running_processes,
    .name = "VE",
    .processing_unit = gpu_processing_unit_ve,
};

__attribute__((constructor)) static void init_extract_gpuinfo_ve(void) { register_gpu_vendor(&gpu_vendor_ve); }

static bool ve_node_present[VE_MAX_NODES];
static unsigned ve_node_count = 0;
static struct gpu_info_ve *ve_infos = NULL;

/* vecmd info cache ----------------------------------------------------------------------------- */
static struct ve_node_sample vecmd_samples[VE_MAX_NODES];
static nvtop_time vecmd_last_refresh;
static bool vecmd_refreshed_once = false;

static void ve_vecmd_parse(FILE *fp) {
  char line[512];
  int cur = -1;

  while (fgets(line, sizeof(line), fp)) {
    char *p = line;
    while (*p && isspace((unsigned char)*p))
      p++;

    // Section header per VE: "[VE0 : ALL  ]"
    if (strncmp(p, "[VE", 3) == 0) {
      char *end = NULL;
      long n = strtol(p + 3, &end, 10);
      cur = (end != p + 3 && n >= 0 && n < VE_MAX_NODES) ? (int)n : -1;
      continue;
    }
    if (cur < 0)
      continue;

    char *colon = strchr(p, ':');
    if (!colon)
      continue;

    // Label is the text before ':' with trailing blanks removed
    char *lend = colon;
    while (lend > p && isspace((unsigned char)lend[-1]))
      lend--;
    size_t llen = (size_t)(lend - p);
    char label[64];
    if (llen == 0 || llen >= sizeof(label))
      continue;
    memcpy(label, p, llen);
    label[llen] = '\0';

    struct ve_node_sample *s = &vecmd_samples[cur];
    if (strcmp(label, "VE Type") == 0) {
      // Value is text, e.g. "10C"
      const char *t = colon + 1;
      while (*t && isspace((unsigned char)*t))
        t++;
      size_t tl = strcspn(t, " \t\r\n");
      if (tl > 0 && tl < sizeof(s->type)) {
        memcpy(s->type, t, tl);
        s->type[tl] = '\0';
        s->type_valid = true;
      }
      continue;
    }
    if (strcmp(label, "Negotiated Link Width") == 0) {
      // Value is written as "x16"
      const char *w = colon + 1;
      while (*w && (isspace((unsigned char)*w) || *w == 'x'))
        w++;
      long lw = strtol(w, NULL, 10);
      if (lw > 0) {
        s->pcie_width = (unsigned)lw;
        s->pcie_width_valid = true;
      }
      continue;
    }

    char *vend = NULL;
    double value = strtod(colon + 1, &vend);
    if (vend == colon + 1)
      continue;

    if (strcmp(label, "Current Link Speed") == 0) {
      // GT/s: 2.5 / 5 / 8 / 16 / 32; truncation keeps 2.5 as 2 for nvtop_pcie_gen_from_link_speed()
      s->pcie_gen = nvtop_pcie_gen_from_link_speed((unsigned)value);
      s->pcie_gen_valid = s->pcie_gen > 0;
    } else if (strncmp(label, "Chip Core", 9) == 0) {
      unsigned t = (unsigned)lround(value);
      if (!s->temp_valid || t > s->temp_max) {
        s->temp_max = t;
        s->temp_valid = true;
      }
    } else if (strcmp(label, "Fan speed") == 0) {
      s->fan_rpm = (unsigned)lround(value);
      s->fan_valid = true;
    } else if (strcmp(label, "Core Clock") == 0) {
      s->core_clock_mhz = (unsigned)lround(value);
      s->core_clock_valid = true;
    } else if (strcmp(label, "Memory Clock") == 0) {
      s->mem_clock_mhz = (unsigned)lround(value);
      s->mem_clock_valid = true;
    } else if (strcmp(label, "Memory Size") == 0) {
      s->mem_total = (unsigned long long)lround(value) * 1024ULL * 1024ULL * 1024ULL; // reported in GB
      s->mem_size_valid = true;
    } else if (strcmp(label, "AUX  12V V") == 0) {
      s->aux_v = value;
      s->rail_mask |= VE_RAIL_AUX_V;
    } else if (strcmp(label, "AUX  12V C") == 0) {
      s->aux_c = value;
      s->rail_mask |= VE_RAIL_AUX_C;
    } else if (strcmp(label, "Edge 12V V") == 0) {
      s->edge_v = value;
      s->rail_mask |= VE_RAIL_EDGE_V;
    } else if (strcmp(label, "Edge 12V C") == 0) {
      s->edge_c = value;
      s->rail_mask |= VE_RAIL_EDGE_C;
    }
  }
}

static void ve_vecmd_refresh(void) {
  nvtop_time now;
  nvtop_get_current_time(&now);
  if (vecmd_refreshed_once && nvtop_difftime_u64(vecmd_last_refresh, now) < VE_VECMD_CACHE_NS)
    return;
  vecmd_last_refresh = now;
  vecmd_refreshed_once = true;

  memset(vecmd_samples, 0, sizeof(vecmd_samples));
  FILE *fp = popen("LC_ALL=C " VE_VECMD_PATH " info 2>/dev/null", "r");
  if (!fp)
    return;
  ve_vecmd_parse(fp);
  pclose(fp);

  // Board power is V * I of the two 12V input rails (V * mA = mW)
  for (unsigned n = 0; n < VE_MAX_NODES; n++) {
    struct ve_node_sample *s = &vecmd_samples[n];
    s->power_valid = (s->rail_mask & VE_RAIL_ALL) == VE_RAIL_ALL;
  }
}

/* Long-running ve-mpstat / ve-top streams (one child per VE node and tool) ----------------------- */
enum ve_stream_kind {
  VE_STREAM_MPSTAT = 0,
  VE_STREAM_TOP,
  VE_STREAM_KIND_COUNT,
};

// One row of ve-top's process table. PID is the host ve_exec PID.
struct ve_top_proc {
  pid_t pid;
  unsigned cpu_pct; // ve-top %CPU: 100 means one VE core fully used
  unsigned long long res_bytes;
  char user[32];
  char cmd[64];
};

struct ve_stream {
  bool started;
  enum ve_stream_kind kind;
  pid_t pid;
  int fd;
  unsigned node;
  char buf[VE_STREAM_BUF_LEN];
  size_t len;
  // mpstat: utilisation of the whole VE
  unsigned util_rate;
  bool util_valid;
  nvtop_time util_time;
  // top: memory in use (bytes)
  unsigned long long mem_used;
  bool mem_valid;
  nvtop_time mem_time;
  // top: process table. A frame is published when the next "ve-top" header arrives.
  bool frame_open;
  struct ve_top_proc frame[VE_TOP_PROCS];
  unsigned frame_count;
  struct ve_top_proc procs[VE_TOP_PROCS];
  unsigned procs_count;
  bool procs_valid;
  nvtop_time procs_time;
};

static struct ve_stream streams[VE_MAX_NODES][VE_STREAM_KIND_COUNT];

static void ve_stream_consume_mpstat(struct ve_stream *st, const char *line) {
  // Only the aggregate row is used: "HH:MM:SS  all  %usr ... %idle"
  if (strncmp(line, "Average:", 8) == 0 || strncmp(line, "VE Node:", 8) == 0)
    return;

  char buf[VE_STREAM_BUF_LEN];
  snprintf(buf, sizeof(buf), "%s", line);
  char *tok[VE_STREAM_MAX_TOKENS];
  int nt = 0;
  for (char *s = strtok(buf, " \t\r"); s && nt < VE_STREAM_MAX_TOKENS; s = strtok(NULL, " \t\r"))
    tok[nt++] = s;
  if (nt < 3 || strcmp(tok[1], "all") != 0)
    return;

  char *end = NULL;
  double idle = strtod(tok[nt - 1], &end);
  if (end == tok[nt - 1] || *end != '\0')
    return;

  double util = 100.0 - idle;
  if (util < 0.0)
    util = 0.0;
  if (util > 100.0)
    util = 100.0;
  st->util_rate = (unsigned)lround(util);
  st->util_valid = true;
  nvtop_get_current_time(&st->util_time);
}

// "3148923 matsuo - - 290816 262144 0 R 100.0 1.0 0:04.10 busy"
// RES is KiB. %CPU follows ve-top: one fully used core is 100, eight cores is 800.
static bool ve_parse_top_proc(const char *line, struct ve_top_proc *out) {
  char buf[VE_STREAM_BUF_LEN];
  snprintf(buf, sizeof(buf), "%s", line);
  char *tok[12];
  int nt = 0;
  char *rest = buf;
  while (nt < 12 && rest && *rest) {
    while (*rest == ' ' || *rest == '\t')
      rest++;
    if (!*rest)
      break;
    tok[nt++] = rest;
    if (nt == 12)
      break;
    rest = strpbrk(rest, " \t");
    if (rest)
      *rest++ = '\0';
  }
  if (nt < 12)
    return false;

  char *end = NULL;
  long pid = strtol(tok[0], &end, 10);
  if (end == tok[0] || *end != '\0' || pid <= 0)
    return false;
  double res = strtod(tok[5], &end);
  if (end == tok[5] || res < 0.0)
    return false;
  double cpu = strtod(tok[8], &end);
  if (end == tok[8])
    return false;

  memset(out, 0, sizeof(*out));
  out->pid = (pid_t)pid;
  out->res_bytes = (unsigned long long)res * 1024ULL;
  if (cpu < 0.0)
    cpu = 0.0;
  out->cpu_pct = (unsigned)lround(cpu);
  if (out->cpu_pct > 999)
    out->cpu_pct = 999;
  snprintf(out->user, sizeof(out->user), "%s", tok[1]);
  snprintf(out->cmd, sizeof(out->cmd), "%s", tok[11]);
  return true;
}

static void ve_stream_consume_top(struct ve_stream *st, const char *line) {
  // A new screen starts here; the previous screen's process list is now complete.
  if (strncmp(line, "ve-top", 6) == 0) {
    if (st->frame_open) {
      memcpy(st->procs, st->frame, st->frame_count * sizeof(*st->procs));
      st->procs_count = st->frame_count;
      st->procs_valid = true;
      nvtop_get_current_time(&st->procs_time);
    }
    st->frame_count = 0;
    st->frame_open = true;
    return;
  }

  struct ve_top_proc proc;
  if (ve_parse_top_proc(line, &proc)) {
    if (st->frame_count < VE_TOP_PROCS)
      st->frame[st->frame_count++] = proc;
    return;
  }

  // "MiB Mem :  24576.0 total,  24510.0 free,     66.0 used,      0.0 buff/cache"
  if (strncmp(line, "MiB Mem", 7) != 0)
    return;

  char buf[VE_STREAM_BUF_LEN];
  snprintf(buf, sizeof(buf), "%s", line);
  char *tok[VE_STREAM_MAX_TOKENS];
  int nt = 0;
  for (char *s = strtok(buf, " \t\r,"); s && nt < VE_STREAM_MAX_TOKENS; s = strtok(NULL, " \t\r,"))
    tok[nt++] = s;

  for (int i = 1; i < nt; i++) {
    if (strcmp(tok[i], "used") != 0)
      continue;
    char *end = NULL;
    double mib = strtod(tok[i - 1], &end);
    if (end == tok[i - 1] || mib < 0.0)
      return;
    st->mem_used = (unsigned long long)(mib * 1024.0 * 1024.0);
    st->mem_valid = true;
    nvtop_get_current_time(&st->mem_time);
    return;
  }
}

static void ve_stream_consume_line(struct ve_stream *st, const char *line) {
  if (st->kind == VE_STREAM_MPSTAT)
    ve_stream_consume_mpstat(st, line);
  else
    ve_stream_consume_top(st, line);
}

static void ve_stream_close(struct ve_stream *st) {
  if (st->fd >= 0) {
    close(st->fd);
    st->fd = -1;
  }
  if (st->pid > 0) {
    kill(st->pid, SIGTERM);
    waitpid(st->pid, NULL, 0);
    st->pid = -1;
  }
}

static void ve_stream_start(struct ve_stream *st, unsigned node, enum ve_stream_kind kind) {
  st->started = true;
  st->kind = kind;
  st->node = node;
  st->fd = -1;
  st->pid = -1;
  st->len = 0;
  st->util_valid = false;
  st->mem_valid = false;

  int fds[2];
  if (pipe(fds) != 0)
    return;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return;
  }
  if (pid == 0) {
    // Do not leak nvtop's own descriptors (e.g. GPU device fds) into the child
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536)
      maxfd = 65536;
    int devnull = open("/dev/null", O_WRONLY);
    dup2(fds[1], STDOUT_FILENO);
    if (devnull >= 0)
      dup2(devnull, STDERR_FILENO);
    for (int fd = (int)maxfd - 1; fd >= 3; fd--)
      close(fd);
    setenv("LC_ALL", "C", 1);
    char node_str[16];
    snprintf(node_str, sizeof(node_str), "%u", node);
    setenv("VE_NODE_NUMBER", node_str, 1); // -N does not select VE1 here; the environment variable does
    if (kind == VE_STREAM_MPSTAT)
      execl(VE_MPSTAT_PATH, "ve-mpstat", "1", (char *)NULL);
    else
      execl(VE_TOP_PATH, "ve-top", "-b", "-d", "1", (char *)NULL);
    _exit(127);
  }
  close(fds[1]);
  int flags = fcntl(fds[0], F_GETFL, 0);
  fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  st->pid = pid;
  st->fd = fds[0];
}

static void ve_stream_pump(struct ve_stream *st) {
  if (st->fd < 0)
    return;
  for (;;) {
    ssize_t n = read(st->fd, st->buf + st->len, sizeof(st->buf) - 1 - st->len);
    if (n == 0) {
      // child exited
      ve_stream_close(st);
      return;
    }
    if (n < 0)
      return; // EAGAIN: nothing more to read right now

    st->len += (size_t)n;
    st->buf[st->len] = '\0';

    char *start = st->buf;
    char *nl;
    while ((nl = memchr(start, '\n', (size_t)(st->buf + st->len - start))) != NULL) {
      *nl = '\0';
      ve_stream_consume_line(st, start);
      start = nl + 1;
    }
    size_t rest = (size_t)(st->buf + st->len - start);
    memmove(st->buf, start, rest);
    st->len = rest;
    if (st->len >= sizeof(st->buf) - 1)
      st->len = 0; // overlong partial line: drop it
  }
}

static void ve_streams_stop_all(void) {
  for (unsigned n = 0; n < VE_MAX_NODES; n++) {
    for (unsigned k = 0; k < VE_STREAM_KIND_COUNT; k++) {
      if (streams[n][k].started)
        ve_stream_close(&streams[n][k]);
    }
  }
}

/* Device enumeration --------------------------------------------------------------------------- */
static unsigned ve_enumerate_nodes(void) {
  DIR *d = opendir(VE_SYSFS_CLASS_DIR);
  if (!d)
    return 0;
  unsigned count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "ve", 2) != 0 || e->d_name[2] == '\0')
      continue;
    char *end = NULL;
    long n = strtol(e->d_name + 2, &end, 10);
    if (*end != '\0' || n < 0 || n >= VE_MAX_NODES || ve_node_present[n])
      continue;
    ve_node_present[n] = true;
    count++;
  }
  closedir(d);
  return count;
}

bool gpuinfo_ve_init(void) {
  ve_node_count = ve_enumerate_nodes();
  if (ve_node_count == 0)
    return false;
  for (unsigned n = 0; n < VE_MAX_NODES; n++) {
    if (!ve_node_present[n])
      continue;
    ve_stream_start(&streams[n][VE_STREAM_MPSTAT], n, VE_STREAM_MPSTAT);
    ve_stream_start(&streams[n][VE_STREAM_TOP], n, VE_STREAM_TOP);
  }
  return true;
}

void gpuinfo_ve_shutdown(void) {
  ve_streams_stop_all();
  free(ve_infos);
  ve_infos = NULL;
  ve_node_count = 0;
}

const char *gpuinfo_ve_last_error_string(void) { return "VE: no error"; }

bool gpuinfo_ve_get_device_handles(struct list_head *devices_list, unsigned *count) {
  *count = 0;
  if (ve_node_count == 0)
    return false;
  ve_infos = (struct gpu_info_ve *)calloc(ve_node_count, sizeof(*ve_infos));
  if (!ve_infos)
    return false;

  for (unsigned n = 0; n < VE_MAX_NODES; n++) {
    if (!ve_node_present[n])
      continue;
    struct gpu_info_ve *dev = &ve_infos[*count];
    dev->base.vendor = &gpu_vendor_ve;
    dev->node = n;
    snprintf(dev->base.pdev, PDEV_LEN, "VE%u", n);
    dev->base.processes_count = 0;
    dev->base.processes = NULL;
    dev->base.processes_array_size = 0;
    list_add_tail(&dev->base.list, devices_list);
    (*count)++;
  }
  return true;
}

void gpuinfo_ve_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_ve *dev = container_of(_gpu_info, struct gpu_info_ve, base);
  struct gpuinfo_static_info *static_info = &dev->base.static_info;
  static_info->integrated_graphics = false;
  static_info->encode_decode_shared = false;
  RESET_ALL(static_info->valid);
  ve_vecmd_refresh();
  const struct ve_node_sample *s = dev->node < VE_MAX_NODES ? &vecmd_samples[dev->node] : NULL;
  if (s && s->type_valid)
    snprintf(static_info->device_name, MAX_DEVICE_NAME, "NEC SX-Aurora TSUBASA Type %s", s->type);
  else
    snprintf(static_info->device_name, MAX_DEVICE_NAME, "NEC SX-Aurora VE%u", dev->node);
  SET_VALID(gpuinfo_device_name_valid, static_info->valid);
}

static void ve_pump_node(unsigned node) {
  if (node >= VE_MAX_NODES)
    return;
  for (unsigned k = 0; k < VE_STREAM_KIND_COUNT; k++) {
    if (streams[node][k].started)
      ve_stream_pump(&streams[node][k]);
  }
}

void gpuinfo_ve_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_ve *dev = container_of(_gpu_info, struct gpu_info_ve, base);
  struct gpuinfo_dynamic_info *dyn = &dev->base.dynamic_info;

  ve_vecmd_refresh();
  ve_pump_node(dev->node);
  RESET_ALL(dyn->valid);
  if (dev->node >= VE_MAX_NODES)
    return;

  const struct ve_node_sample *s = &vecmd_samples[dev->node];
  if (s->core_clock_valid) {
    SET_GPUINFO_DYNAMIC(dyn, gpu_clock_speed, s->core_clock_mhz);
    SET_GPUINFO_DYNAMIC(dyn, gpu_clock_speed_max, s->core_clock_mhz);
  }
  if (s->mem_clock_valid) {
    SET_GPUINFO_DYNAMIC(dyn, mem_clock_speed, s->mem_clock_mhz);
    SET_GPUINFO_DYNAMIC(dyn, mem_clock_speed_max, s->mem_clock_mhz);
  }
  if (s->mem_size_valid) {
    SET_GPUINFO_DYNAMIC(dyn, total_memory, s->mem_total);
  }

  // Memory in use comes from ve-top; free memory is derived from the total size
  const struct ve_stream *top = &streams[dev->node][VE_STREAM_TOP];
  if (top->mem_valid && s->mem_size_valid) {
    nvtop_time now;
    nvtop_get_current_time(&now);
    if (nvtop_difftime_u64(top->mem_time, now) <= VE_SAMPLE_MAX_AGE_NS) {
      unsigned long long used = top->mem_used > s->mem_total ? s->mem_total : top->mem_used;
      SET_GPUINFO_DYNAMIC(dyn, used_memory, used);
      SET_GPUINFO_DYNAMIC(dyn, free_memory, s->mem_total - used);
    }
  }
  if (s->pcie_gen_valid) {
    SET_GPUINFO_DYNAMIC(dyn, pcie_link_gen, s->pcie_gen);
  }
  if (s->pcie_width_valid) {
    SET_GPUINFO_DYNAMIC(dyn, pcie_link_width, s->pcie_width);
  }
  if (s->temp_valid) {
    SET_GPUINFO_DYNAMIC(dyn, gpu_temp, s->temp_max);
  }
  if (s->fan_valid) {
    SET_GPUINFO_DYNAMIC(dyn, fan_rpm, s->fan_rpm);
    unsigned fan_pct = (unsigned)(s->fan_rpm * 100u / VE_FAN_MAX_RPM);
    SET_GPUINFO_DYNAMIC(dyn, fan_speed, fan_pct > 100u ? 100u : fan_pct);
  }
  if (s->power_valid) {
    // V * mA = mW
    double power_mw = s->aux_v * s->aux_c + s->edge_v * s->edge_c;
    if (power_mw < 0.0)
      power_mw = 0.0;
    SET_GPUINFO_DYNAMIC(dyn, power_draw, (unsigned)lround(power_mw));
  }
}

void gpuinfo_ve_refresh_utilisation_rate(struct gpu_info *_gpu_info) {
  struct gpu_info_ve *dev = container_of(_gpu_info, struct gpu_info_ve, base);
  if (dev->node >= VE_MAX_NODES)
    return;
  ve_pump_node(dev->node);

  const struct ve_stream *mp = &streams[dev->node][VE_STREAM_MPSTAT];
  if (!mp->util_valid)
    return;

  nvtop_time now;
  nvtop_get_current_time(&now);
  if (nvtop_difftime_u64(mp->util_time, now) > VE_SAMPLE_MAX_AGE_NS)
    return;
  SET_GPUINFO_DYNAMIC(&dev->base.dynamic_info, gpu_util_rate, mp->util_rate);
}

void gpuinfo_ve_refresh_running_processes(struct gpu_info *_gpu_info) {
  struct gpu_info_ve *dev = container_of(_gpu_info, struct gpu_info_ve, base);
  _gpu_info->processes_count = 0;
  if (dev->node >= VE_MAX_NODES)
    return;

  // ve-top lists every VE process, including other users' (it reads VEOS, not /proc).
  ve_pump_node(dev->node);
  struct ve_stream *top = &streams[dev->node][VE_STREAM_TOP];
  if (!top->procs_valid)
    return;
  nvtop_time now;
  nvtop_get_current_time(&now);
  if (nvtop_difftime_u64(top->procs_time, now) > VE_SAMPLE_MAX_AGE_NS)
    return;

  if (top->procs_count > _gpu_info->processes_array_size) {
    struct gpu_process *np = realloc(_gpu_info->processes, top->procs_count * sizeof(*np));
    if (!np)
      return;
    _gpu_info->processes = np;
    _gpu_info->processes_array_size = top->procs_count;
  }
  for (unsigned i = 0; i < top->procs_count; i++) {
    struct gpu_process *p = &_gpu_info->processes[i];
    const struct ve_top_proc *src = &top->procs[i];
    memset(p, 0, sizeof(*p));
    p->type = gpu_process_compute;
    p->pid = src->pid;
    SET_GPUINFO_PROCESS(p, gpu_usage, src->cpu_pct);
    SET_GPUINFO_PROCESS(p, gpu_memory_usage, src->res_bytes);
    SET_GPUINFO_PROCESS(p, user_name, (char *)src->user);
    SET_GPUINFO_PROCESS(p, cmdline, (char *)src->cmd);
  }
  _gpu_info->processes_count = top->procs_count;
}
