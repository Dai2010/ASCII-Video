from edit_helpers import replace_function
from pathlib import Path
p=Path('src/encoder.c');s=p.read_text();s=s.replace('if (miku_exit_ok(system(cmd))) {\n\t\t\t\tif (rename(tmp, output) == 0)\n\t\t\t\t\tdone = 1;\n\t\t\t}', 'if (miku_exit_ok(system(cmd)) && rename(tmp, output) == 0)\n                                done = 1;');p.write_text(s)
replace_function('src/encoder.c','vaapi_device_arg','''
/*
 * 扫描结果缓存到静态字符串，返回借用指针或 NULL；仅首次调用扫描。
 * 设备名仅接受 renderD 前缀；长度不足以容纳完整参数时跳过该项。
 * 缓存不支持并发初始化，编码器探测由主线程串行执行。
 */
static const char *vaapi_device_arg(void)
{
        static char buffer[128];
        static int scanned;
        const char *prefix = "-vaapi_device /dev/dri/";
        struct dirent *entry;
        DIR *directory;
        size_t length;

        if (scanned)
                return buffer[0] ? buffer : NULL;
        scanned = 1;
        directory = opendir("/dev/dri");
        if (!directory)
                return NULL;
        while ((entry = readdir(directory)) != NULL) {
                if (strncmp(entry->d_name, "renderD", 7) != 0)
                        continue;
                length = strlen(entry->d_name);
                if (strlen(prefix) + length >= sizeof(buffer))
                        continue;
                memcpy(buffer, prefix, strlen(prefix));
                memcpy(buffer + strlen(prefix), entry->d_name, length + 1);
                break;
        }
        closedir(directory);
        return buffer[0] ? buffer : NULL;
}
''')
replace_function('src/encoder.c','miku_choose_profile','''
/* 档位信息复制到结果，不持有档位表之外的内存。 */
static void profile_from_tier(struct profile *profile, int tier, int source_fps)
{
        const struct tier *specification = &miku_tiers[tier];

        snprintf(profile->name, sizeof(profile->name), "%s", specification->name);
        profile->width = specification->width;
        profile->height = specification->height;
        profile->fps = specification->fps;
        if (source_fps > 0 && source_fps < profile->fps)
                profile->fps = source_fps;
}

/*
 * 高到低探测并选择进 CPU/内存预算的首档，否则保留最低可用档。
 * 预算是每秒视频可占用的 CPU 秒数，内存未知时不作内存筛选。
 */
static int choose_auto_profile(int source_fps, struct profile *profile)
{
        struct profile candidate = {0};
        long memory = miku_total_memory_mb();
        double budget = miku_cpu_cores() * CPU_BUDGET_RATIO;
        int i;

        memset(profile, 0, sizeof(*profile));
        for (i = 0; i < TIER_COUNT; i++) {
                profile_from_tier(&candidate, i, source_fps);
                candidate.enc = miku_pick_encoder(candidate.width,
                                                  candidate.height,
                                                  miku_tiers[i].fps);
                if (!candidate.enc)
                        continue;
                *profile = candidate;
                if (memory > 0 && memory < miku_tiers[i].min_mem_mb)
                        continue;
                if (profile_cpu(&candidate) <= budget)
                        return 1;
        }
        if (!profile->enc)
                return 0;
        profile->enc = miku_pick_encoder(profile->width, profile->height,
                                         profile->fps);
        return profile->enc != NULL;
}

/*
 * 解析显式档位/尺寸或执行自动选档；结果借用静态编码器候选。
 * 当前内部约定为 1 成功、0 无可用组合，外部转换入口统一转换为错误码。
 * src_fps<=0 表示未知；有效时限制输出不超过源帧率。
 */
int miku_choose_profile(const char *quality, int src_fps, struct profile *p)
{
        const char *name = quality && *quality ? quality : "auto";
        int tier;

        if (strcmp(name, "auto") == 0)
                return choose_auto_profile(src_fps, p);
        if (miku_parse_custom_size(name, &p->width, &p->height, &p->fps)) {
                if (p->fps > 0)
                        snprintf(p->name, sizeof(p->name), "%dx%d@%d",
                                 p->width, p->height, p->fps);
                else
                        snprintf(p->name, sizeof(p->name), "%dx%d",
                                 p->width, p->height);
        } else {
                for (tier = 0; tier < TIER_COUNT; tier++) {
                        if (strcmp(name, miku_tiers[tier].name) == 0)
                                break;
                }
                if (tier == TIER_COUNT)
                        return 0;
                profile_from_tier(p, tier, src_fps);
        }
        if (src_fps > 0 && (p->fps <= 0 || src_fps < p->fps))
                p->fps = src_fps;
        if (p->fps < 1)
                p->fps = miku_tiers[TIER_COUNT - 1].fps;
        p->enc = miku_pick_encoder(p->width, p->height, p->fps);
        return p->enc != NULL;
}
''')
