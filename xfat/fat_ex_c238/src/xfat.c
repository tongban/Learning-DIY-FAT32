/**
 * 本源码配套的课程为 - 从0到1动手写FAT32文件系统。每个例程对应一个课时，尽可能注释。
 * 作者：李述铜
 * 课程网址：http://01ketang.cc
 * 版权声明：本源码非开源，二次开发，或其它商用前请联系作者。
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xfat.h"
#include "xdisk.h"

extern u8_t temp_buffer[512];      // todo: 缓存优化

// 内置的.和..文件名             "12345678ext"
#define DOT_FILE                ".          "
#define DOT_DOT_FILE            "..         "

#define is_path_sep(ch)         (((ch) == '\\') || ((ch == '/')))       // 判断是否是文件名分隔符
#define file_get_disk(file)     ((file)->xfat->disk_part->disk)         // 获取disk结构
#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // 获取disk结构
#define to_sector(disk, offset)     ((offset) / (disk)->sector_size)    // 将依稀转换为扇区号
#define to_sector_offset(disk, offset)   ((offset) % (disk)->sector_size)   // 获取扇区中的相对偏移
#define to_cluster_offset(xfat, pos)      ((pos) % ((xfat)->cluster_byte_size)) // 获取簇中的相对偏移

/**
 * 从dbr中解析出fat相关配置参数
 * @param dbr 读取的设备dbr
 * @return
 */
static xfat_err_t parse_fat_header (xfat_t * xfat, dbr_t * dbr) {
    xdisk_part_t * xdisk_part = xfat->disk_part;

    // 解析DBR参数，解析出有用的参数
    xfat->root_cluster = dbr->fat32.BPB_RootClus;
    xfat->fat_tbl_sectors = dbr->fat32.BPB_FATSz32;

    // 如果禁止FAT镜像，只刷新一个FAT表
    // disk_part->start_block为该分区的绝对物理扇区号，所以不需要再加上Hidden_sector
    if (dbr->fat32.BPB_ExtFlags & (1 << 7)) {
        u32_t table = dbr->fat32.BPB_ExtFlags & 0xF;
        xfat->fat_start_sector = dbr->bpb.BPB_RsvdSecCnt + xdisk_part->start_sector + table * xfat->fat_tbl_sectors;
        xfat->fat_tbl_nr = 1;
    } else {
        xfat->fat_start_sector = dbr->bpb.BPB_RsvdSecCnt + xdisk_part->start_sector;
        xfat->fat_tbl_nr = dbr->bpb.BPB_NumFATs;
    }

    xfat->sec_per_cluster = dbr->bpb.BPB_SecPerClus;
    xfat->total_sectors = dbr->bpb.BPB_TotSec32;
    xfat->cluster_byte_size = xfat->sec_per_cluster * dbr->bpb.BPB_BytsPerSec;

    return FS_ERR_OK;
}

/**
 * 初始化FAT项
 * @param xfat xfat结构
 * @param disk_part 分区结构
 * @return
 */
xfat_err_t xfat_open(xfat_t * xfat, xdisk_part_t * xdisk_part) {
    dbr_t * dbr = (dbr_t *)temp_buffer;
    xdisk_t * xdisk = xdisk_part->disk;
    xfat_err_t err;

    xfat->disk_part = xdisk_part;

    // 读取dbr参数区
    err = xdisk_read_sector(xdisk, (u8_t *) dbr, xdisk_part->start_sector, 1);
    if (err < 0) {
        return err;
    }

    // 解析dbr参数中的fat相关信息
    err = parse_fat_header(xfat, dbr);
    if (err < 0) {
        return err;
    }

    // 先一次性全部读取FAT表: todo: 优化
    xfat->fat_buffer = (u8_t *)malloc(xfat->fat_tbl_sectors * xdisk->sector_size);
    err = xdisk_read_sector(xdisk, (u8_t *)xfat->fat_buffer, xfat->fat_start_sector, xfat->fat_tbl_sectors);
    if (err < 0) {
        return err;
    }

    return FS_ERR_OK;
}

/**
 * 获取指定簇号的第一个扇区编号
 * @param xfat xfat结构
 * @param cluster_no  簇号
 * @return 扇区号
 */
u32_t cluster_fist_sector(xfat_t *xfat, u32_t cluster_no) {
    u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;
    return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;    // 前两个簇号保留
}

/**
 * 检查指定簇是否可用，非占用或坏簇
 * @param cluster 待检查的簇
 * @return
 */
int is_cluster_valid(u32_t cluster) {
    cluster &= 0x0FFFFFFF;
    return (cluster < 0x0FFFFFF0) && (cluster >= 0x2);     // 值是否正确
}

/**
 * 获取指定簇的下一个簇
 * @param xfat xfat结构
 * @param curr_cluster_no
 * @param next_cluster
 * @return
 */
xfat_err_t get_next_cluster(xfat_t * xfat, u32_t curr_cluster_no, u32_t * next_cluster) {
    if (is_cluster_valid(curr_cluster_no)) {
        cluster32_t * cluster32_buf = (cluster32_t *)xfat->fat_buffer;
        *next_cluster = cluster32_buf[curr_cluster_no].s.next;
    } else {
        *next_cluster = CLUSTER_INVALID;
    }

    return FS_ERR_OK;
}

/**
 * 读取一个簇的内容到指定缓冲区
 * @param xfat xfat结构
 * @param buffer 数据存储的缓冲区
 * @param cluster 读取的起始簇号
 * @param count 读取的簇数量
 * @return
 */
xfat_err_t read_cluster(xfat_t *xfat, u8_t *buffer, u32_t cluster, u32_t count) {
    xfat_err_t err = 0;
    u32_t i = 0;
    u8_t * curr_buffer = buffer;
    u32_t curr_sector = cluster_fist_sector(xfat, cluster);

    for (i = 0; i < count; i++) {
        err = xdisk_read_sector(xfat_get_disk(xfat), curr_buffer, curr_sector, xfat->sec_per_cluster);
        if (err < 0) {
            return err;
        }

        curr_buffer += xfat->cluster_byte_size;
        curr_sector += xfat->sec_per_cluster;
    }

    return FS_ERR_OK;
}

/**
 * 将指定的name按FAT 8+3命名转换
 * @param dest_name
 * @param my_name
 * @return
 */
static xfat_err_t to_sfn(char* dest_name, const char* my_name) {
    int i, name_len;
    char * dest = dest_name;
    const char * ext_dot;
    const char * p;
    int ext_existed;

    memset(dest, ' ', SFN_LEN);

    // 跳过开头的分隔符
    while (is_path_sep(*my_name)) {
        my_name++;
    }

    // 找到第一个斜杠之前的字符串，将ext_dot定位到那里，且记录有效长度
    ext_dot = my_name;
    p = my_name;
    name_len = 0;
    while ((*p != '\0') && !is_path_sep(*p)) {
        if (*p == '.') {
            ext_dot = p;
        }
        p++;
        name_len++;
    }

    // 如果文件名以.结尾，意思就是没有扩展名？
    // todo: 长文件名处理?
    ext_existed = (ext_dot > my_name) && (ext_dot < (my_name + name_len - 1));

    // 遍历名称，逐个复制字符, 算上.分隔符，最长12字节，如果分离符，则只应有
    p = my_name;
    for (i = 0; (i < SFN_LEN) && (*p != '\0') && !is_path_sep(*p); i++) {
        if (ext_existed) {
            if (p == ext_dot) {
                dest = dest_name + 8;
                p++;
                i--;
                continue;
            }
            else if (p < ext_dot) {
                *dest++ = toupper(*p++);
            }
            else {
                *dest++ = toupper(*p++);
            }
        }
        else {
            *dest++ = toupper(*p++);
        }
    }
    return FS_ERR_OK;
}

/**
 * 判断两个文件名是否匹配
 * @param name_in_item fatdir中的文件名格式
 * @param my_name 应用可读的文件名格式
 * @return
 */
static u8_t is_filename_match(const char *name_in_dir, const char *to_find_name) {
    char temp_name[SFN_LEN];

    // FAT文件名的比较检测等，全部转换成大写比较
    // 根据目录的大小写配置，将其转换成8+3名称，再进行逐字节比较
    // 但实际显示时，会根据diritem->NTRes进行大小写转换
    to_sfn(temp_name, to_find_name);
    return memcmp(temp_name, name_in_dir, SFN_LEN) == 0;
}

/**
 * 跳过开头的分隔符
 * @param path 目标路径
 * @return
 */
static const char * skip_first_path_sep (const char * path) {
    const char * c = path;

    // 跳过开头的分隔符
    while (is_path_sep(*c)) {
        c++;
    }
    return c;
}

/**
 * 获取子路径
 * @param dir_path 上一级路径
 * @return
 */
const char * get_child_path(const char *dir_path) {
    const char * c = skip_first_path_sep(dir_path);

    // 跳过父目录
    while ((*c != '\0') && !is_path_sep(*c)) {
        c++;
    }

    return (*c == '\0') ? (const char *)0 : c + 1;
}

/**
 * 解析diritem，获取文件类型
 * @param diritem 需解析的diritem
 * @return
 */
static xfile_type_t get_file_type(const diritem_t *diritem) {
    xfile_type_t type;

    if (diritem->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) {
        type = FAT_VOL;
    } else if (diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY) {
        type = FAT_DIR;
    } else {
        type = FAT_FILE;
    }

    return type;
}

/**
 * 复制相应的时间信息到dest中
 * @param dest 指定存储的时间信息结构
 * @param date fat格式的日期
 * @param time fat格式的时间
 * @param mil_sec fat格式的10毫秒
 */
static void copy_date_time(xfile_time_t *dest, const diritem_date_t *date,
                           const diritem_time_t *time, const u8_t mil_sec) {
    if (date) {
        dest->year = (u16_t)(date->year_from_1980 + 1980);
        dest->month = (u8_t)date->month;
        dest->day = (u8_t)date->day;
    } else {
        dest->year = 0;
        dest->month = 0;
        dest->day = 0;
    }

    if (time) {
        dest->hour = (u8_t)time->hour;
        dest->minute = (u8_t)time->minute;
        dest->second = (u8_t)(time->second_2 * 2 + mil_sec / 100);
    } else {
        dest->hour = 0;
        dest->minute = 0;
        dest->second = 0;
    }
}

/**
 * 从fat_dir格式的文件名中拷贝成用户可读的文件名到dest_name
 * @param dest_name 转换后的文件名存储缓冲区
 * @param raw_name fat_dir格式的文件名
 */
static void sfn_to_myname(char *dest_name, const diritem_t * diritem) {
    int i;
    char * dest = dest_name, * raw_name = (char *)diritem->DIR_Name;
    u8_t ext_exist = raw_name[8] != 0x20;
    u8_t scan_len = ext_exist ? SFN_LEN + 1 : SFN_LEN;

    memset(dest_name, 0, X_FILEINFO_NAME_SIZE);

    // 要考虑大小写问题，根据NTRes配置转换成相应的大小写
    for (i = 0; i < scan_len; i++) {
        if (*raw_name == ' ') {
            raw_name++;
        } else if ((i == 8) && ext_exist) {
           *dest++ = '.';
        } else {
            u8_t lower = 0;

            if (((i < 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_BODY_LOWER))
                || ((i > 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_EXT_LOWER))) {
                lower = 1;
            }

            *dest++ = lower ? tolower(*raw_name++) : toupper(*raw_name++);
        }
    }
    *dest = '\0';
}

/**
 * 获取diritem的文件起始簇号
 * @param item
 * @return
 */
static u32_t get_diritem_cluster (diritem_t * item) {
    return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0;
}

/**
 * 将dir_item中相应的文件信息转换存至fs_fileinfo_t中
 * @param info 信息存储的位置
 * @param dir_item fat的diritem
 */
static void copy_file_info(xfileinfo_t *info, const diritem_t * dir_item) {
    sfn_to_myname(info->file_name, dir_item);
    info->size = dir_item->DIR_FileSize;
    info->attr = dir_item->DIR_Attr;
    info->type = get_file_type(dir_item);

    copy_date_time(&info->create_time, &dir_item->DIR_CrtDate, &dir_item->DIR_CrtTime, dir_item->DIR_CrtTimeTeenth);
    copy_date_time(&info->last_acctime, &dir_item->DIR_LastAccDate, (diritem_time_t *) 0, 0) ;
    copy_date_time(&info->modify_time, &dir_item->DIR_WrtDate, &dir_item->DIR_WrtTime, 0);
}

/**
 * 检查文件名和类型是否匹配
 * @param dir_item
 * @param locate_type
 * @return
 */
static u8_t is_locate_type_match (diritem_t * dir_item, u8_t locate_type) {
    u8_t match = 1;

    if ((dir_item->DIR_Attr & DIRITEM_ATTR_SYSTEM) && !(locate_type & XFILE_LOCALE_SYSTEM)) {
        match = 0;  // 不显示系统文件
    } else if ((dir_item->DIR_Attr & DIRITEM_ATTR_HIDDEN) && !(locate_type & XFILE_LOCATE_HIDDEN)) {
        match = 0;  // 不显示隐藏文件
    } else if ((dir_item->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) && !(locate_type & XFILE_LOCATE_VOL)) {
        match = 0;  // 不显示卷标
    } else if ((memcmp(DOT_FILE, dir_item->DIR_Name, SFN_LEN) == 0)
                || (memcmp(DOT_DOT_FILE, dir_item->DIR_Name, SFN_LEN) == 0)) {
        if (!(locate_type & XFILE_LOCATE_DOT)) {
            match = 0;// 不显示dot文件
        }
    } else if (!(locate_type & XFILE_LOCATE_NORMAL)) {
        match = 0;
    }
    return match;
}

/**
 * 查找指定dir_item，并返回相应的结构
 * @param xfat xfat结构
 * @param locate_type 定位的item类型
 * @param dir_cluster dir_item所在的目录数据簇号
 * @param cluster_offset 簇中的偏移
 * @param move_bytes 查找到相应的item项后，相对于最开始传入的偏移值，移动了多少个字节才定位到该item
 * @param path 文件或目录的完整路径
 * @param r_diritem 查找到的diritem项
 * @return
 */
static xfat_err_t locate_file_dir_item(xfat_t *xfat, u8_t locate_type, u32_t *dir_cluster, u32_t *cluster_offset,
                                    const char *path, u32_t *move_bytes, diritem_t **r_diritem) {
    u32_t curr_cluster = *dir_cluster;
    xdisk_t * xdisk = xfat_get_disk(xfat);
    u32_t initial_sector = to_sector(xdisk, *cluster_offset);
    u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);
    u32_t r_move_bytes = 0;

    // cluster
    do {
        u32_t i;
        xfat_err_t err;
        u32_t start_sector = cluster_fist_sector(xfat, curr_cluster);

        for (i = initial_sector; i < xfat->sec_per_cluster; i++) {
            u32_t j;

            err = xdisk_read_sector(xdisk, temp_buffer, start_sector + i, 1);
            if (err < 0) {
                return err;
            }

            for (j = initial_offset / sizeof(diritem_t); j < xdisk->sector_size / sizeof(diritem_t); j++) {
                diritem_t *dir_item = ((diritem_t *) temp_buffer) + j;

                if (dir_item->DIR_Name[0] == DIRITEM_NAME_END) {
                    return FS_ERR_EOF;
                } else if (dir_item->DIR_Name[0] == DIRITEM_NAME_FREE) {
                    r_move_bytes += sizeof(diritem_t);
                    continue;
                } else if (!is_locate_type_match(dir_item, locate_type)) {
                    r_move_bytes += sizeof(diritem_t);
                    continue;
                }

                if ((path == (const char *) 0)
                    || (*path == 0)
                    || is_filename_match((const char *) dir_item->DIR_Name, path)) {

                    u32_t total_offset = i * xdisk->sector_size + j * sizeof(diritem_t);
                    *dir_cluster = curr_cluster;
                    *move_bytes = r_move_bytes + sizeof(diritem_t);
                    *cluster_offset = total_offset;
                    if (r_diritem) {
                        *r_diritem = dir_item;
                    }

                    return FS_ERR_OK;
                }

                r_move_bytes += sizeof(diritem_t);
            }
        }

        err = get_next_cluster(xfat, curr_cluster, &curr_cluster);
        if (err < 0) {
            return err;
        }

        initial_sector = 0;
        initial_offset = 0;
    }while (is_cluster_valid(curr_cluster));

    return FS_ERR_EOF;
}

/**
 * 打开指定dir_cluster开始的簇链中包含的子文件。
 * 如果path为空，则以dir_cluster创建一个打开的目录对像
 * @param xfat xfat结构
 * @param dir_cluster 查找的顶层目录的起始簇链
 * @param file 打开的文件file结构
 * @param path 以dir_cluster所对应的目录为起点的完整路径
 * @return
 */
static xfat_err_t open_sub_file (xfat_t * xfat, u32_t dir_cluster, xfile_t * file, const char * path) {
    u32_t parent_cluster = dir_cluster;
    u32_t parent_cluster_offset = 0;

    path = skip_first_path_sep(path);

    // 如果传入路径不为空，则查看子目录
    // 否则，直接认为dir_cluster指向的是一个目录，用于打开根目录
    if ((path != 0) && (*path != '\0')) {
        diritem_t * dir_item = (diritem_t *)0;
        u32_t file_start_cluster = 0;
        const char * curr_path = path;

       // 找到path对应的起始簇
        while (curr_path != (const char *)0) {
            u32_t moved_bytes = 0;
            dir_item = (diritem_t *)0;

            // 在父目录下查找指定路径对应的文件
            xfat_err_t err = locate_file_dir_item(xfat, XFILE_LOCATE_DOT | XFILE_LOCATE_NORMAL,
                    &parent_cluster, &parent_cluster_offset,curr_path, &moved_bytes, &dir_item);
            if (err < 0) {
                return err;
            }

            if (dir_item == (diritem_t *)0) {
                return FS_ERR_NONE;
            }

            curr_path = get_child_path(curr_path);
            if (curr_path != (const char *)0) {
                parent_cluster = get_diritem_cluster(dir_item);
                parent_cluster_offset = 0;
            } else {
                file_start_cluster = get_diritem_cluster(dir_item);

                // 如果是..且对应根目录，则cluster值为0，需加载正确的值
                if ((memcmp(dir_item->DIR_Name, DOT_DOT_FILE, SFN_LEN) == 0) && (file_start_cluster == 0)) {
                    file_start_cluster = xfat->root_cluster;
                }
            }
        }

        file->size = dir_item->DIR_FileSize;
        file->type = get_file_type(dir_item);
        file->start_cluster = file_start_cluster;
        file->curr_cluster = file_start_cluster;
    } else {
        file->size = 0;
        file->type = FAT_DIR;
        file->start_cluster = parent_cluster;
        file->curr_cluster = parent_cluster;
    }

    file->xfat = xfat;
    file->pos = 0;
    file->err = FS_ERR_OK;
    return FS_ERR_OK;
}

/**
 * 打开指定的文件或目录
 * @param xfat xfat结构
 * @param file 打开的文件或目录
 * @param path 文件或目录所在的完整路径，暂不支持相对路径
 * @return
 */
xfat_err_t xfile_open(xfat_t * xfat, xfile_t * file, const char * path) {
	path = skip_first_path_sep(path);

	// 根目录不存在上级目录
	// 若含有.，直接过滤掉路径
	if (memcmp(path, "..", 2) == 0) {
		return FS_ERR_NONE;
	} else if (memcmp(path, ".", 1) == 0) {
		path++;
	}

    return open_sub_file(xfat, xfat->root_cluster, file, path);
}

/**
 * 在打开的目录下，打开相应的子文件或目录
 * @param dir  已经打开的目录
 * @param sub_file 打开的子文件或目录
 * @param sub_path 以已打开的目录为起点，子文件或目录的完整路径
 * @return
 */
xfat_err_t xfile_open_sub(xfile_t* dir, const char* sub_path, xfile_t* sub_file) {
    if (dir->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    if (memcmp(sub_path, ".", 1) == 0) {
        return FS_ERR_PARAM;
    }

    return open_sub_file(dir->xfat, dir->start_cluster, sub_file, sub_path);
}

/**
 * 返回指定目录下的第一个文件信息
 * @param file 已经打开的文件
 * @param info 第一个文件的文件信息
 * @return
 */
xfat_err_t xdir_first_file (xfile_t * file, xfileinfo_t * info) {
    diritem_t * diritem = (diritem_t *)0;
    xfat_err_t err;
    u32_t moved_bytes = 0;
    u32_t cluster_offset;

    // 仅能用于目录下搜索
    if (file->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    // 重新调整搜索位置
    file->curr_cluster = file->start_cluster;
    file->pos = 0;

    cluster_offset = 0;
    err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL,
            &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem);
    if (err < 0) {
        return err;
    }

    if (diritem == (diritem_t *)0) {
        return FS_ERR_EOF;
    }

    file->pos += moved_bytes;

    // 找到后，拷贝文件信息
    copy_file_info(info, diritem);
    return err;
}

/**
 * 返回指定目录接下来的文件（用于文件遍历)
 * @param file 已经打开的目录
 * @param info 获得的文件信息
 * @return
 */
xfat_err_t xdir_next_file (xfile_t * file, xfileinfo_t * info) {
    xfat_err_t err;
    diritem_t * dir_item = (diritem_t *)0;
    u32_t moved_bytes = 0;
    u32_t cluster_offset;

    // 仅用于目录
    if (file->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    // 搜索文件或目录
    cluster_offset = to_cluster_offset(file->xfat, file->pos);
    err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL,
            &file->curr_cluster, &cluster_offset, "", &moved_bytes, &dir_item);
    if (err != FS_ERR_OK) {
        return err;
    }

    if (dir_item == (diritem_t *)0) {
        return FS_ERR_EOF;
    }

    file->pos += moved_bytes;

    // 移动位置后，可能超过当前簇，更新当前簇位置
    if (cluster_offset + sizeof(diritem_t) >= file->xfat->cluster_byte_size) {
        err = get_next_cluster(file->xfat, file->curr_cluster, &file->curr_cluster);
        if (err < 0) {
            return err;
        }
    }

    copy_file_info(info, dir_item);
    return err;
}

/**
 * 获取文件读写的错误码
 * @param file
 * @return
 */
xfat_err_t xfile_error(xfile_t * file) {
    return file->err;
}

/**
 * 清除文件读写错误状态码
 * @param file
 */
void xfile_clear_err(xfile_t * file) {
    file->err = FS_ERR_OK;
}

/**
 * 从指定的文件中读取相应个数的元素数据
 * @param buffer 数据存储的缓冲区
 * @param elem_size 每次读取的元素字节大小
 * @param count 读取多少个elem_size
 * @param file 要读取的文件
 * @return
 */
xfile_size_t xfile_read(void * buffer, xfile_size_t elem_size, xfile_size_t count, xfile_t * file) {
    u32_t cluster_sector,  sector_offset;
    xdisk_t * disk = file_get_disk(file);
    xfile_size_t r_count_readed = 0;
    xfile_size_t bytes_to_read = count * elem_size;
    u8_t * read_buffer = (u8_t *)buffer;

    // 只允许直接读普通文件
    if (file->type != FAT_FILE) {
        file->err = FS_ERR_FSTYPE;
        return 0;
    }

    // 已经到达文件尾末，不读
    if (file->pos >= file->size) {
        file->err = FS_ERR_EOF;
        return 0;
    }

    // 调整读取量，不要超过文件总量
    if (file->pos + bytes_to_read > file->size) {
        bytes_to_read = file->size - file->pos;
    }

    cluster_sector = to_sector(disk, to_cluster_offset(file->xfat, file->pos));  // 簇中的扇区号
    sector_offset = to_sector_offset(disk, file->pos);  // 扇区偏移位置

    while ((bytes_to_read > 0) && is_cluster_valid(file->curr_cluster)) {
        xfat_err_t err;
        xfile_size_t curr_read_bytes = 0;
        u32_t sector_count = 0;
        u32_t start_sector = cluster_fist_sector(file->xfat, file->curr_cluster) + cluster_sector;

        // 起始非扇区边界对齐, 只读取当前扇区
        // 或者起始为0，但读取量不超过当前扇区，也只读取当前扇区
        // 无论哪种情况，都需要暂存到缓冲区中，然后拷贝到用户缓冲区
        if ((sector_offset != 0) || (!sector_offset && (bytes_to_read < disk->sector_size))) {
            sector_count = 1;
            curr_read_bytes = bytes_to_read;

            // 起始偏移非0，如果跨扇区，只读取当前扇区
            if (sector_offset != 0) {
                if (sector_offset + bytes_to_read > disk->sector_size) {
                    curr_read_bytes = disk->sector_size - sector_offset;
                }
            }

            // 读取整扇区，然后从中间拷贝部分数据到应用缓冲区中
            // todo: 连续多次小批量读时，可能会重新加载同一扇区
            err = xdisk_read_sector(disk, temp_buffer, start_sector, 1);
            if (err < 0) {
                file->err = err;
                return 0;
            }

            memcpy(read_buffer, temp_buffer + sector_offset, curr_read_bytes);
            read_buffer += curr_read_bytes;
            bytes_to_read -= curr_read_bytes;
        } else {
            // 起始为0，且读取量超过1个扇区，连续读取多扇区
            sector_count = (u32_t)to_sector(disk, bytes_to_read);

            // 如果超过一簇，则只读取当前簇
            // todo: 这里可以再优化一下，如果簇连续的话，实际是可以连读多簇的
            if ((cluster_sector + sector_count) > file->xfat->sec_per_cluster) {
                sector_count = file->xfat->sec_per_cluster - cluster_sector;
            }

            err = xdisk_read_sector(disk, read_buffer, start_sector, sector_count);
            if (err != FS_ERR_OK) {
                file->err = err;
                return 0;
            }

            curr_read_bytes = sector_count * disk->sector_size;
            read_buffer += curr_read_bytes;
            bytes_to_read -= curr_read_bytes;
        }
        r_count_readed += curr_read_bytes;

        // 校正下次读取位置
        sector_offset += (u32_t)curr_read_bytes;
        if (sector_offset >= disk->sector_size) {
            sector_offset = 0;

            cluster_sector += sector_count;
            if (cluster_sector >= file->xfat->sec_per_cluster) {
                cluster_sector = 0;

                err = get_next_cluster(file->xfat, file->curr_cluster, &file->curr_cluster);
                if (err != FS_ERR_OK) {
                    file->err = err;
                    return 0;
                }
            }
        }
        file->pos += (u32_t)curr_read_bytes;
    }

    file->err = is_cluster_valid(file->curr_cluster) ? FS_ERR_OK : FS_ERR_EOF;
    return r_count_readed / elem_size;
}

/**
 * 文件是否已经读写到末尾
 * @param file 查询的文件
 * @return
 */
xfat_err_t xfile_eof(xfile_t * file) {
    return (file->pos >= file->size) ? FS_ERR_EOF : FS_ERR_OK;
}

/**
 * 返回当前文件的位置
 * @param file 已经打开的文件
 * @return
 */
xfile_size_t xfile_tell(xfile_t * file) {
    return file->pos;
}

/**
 * 调整文件当前的读写位置
 * @param file 已经打开的文件
 * @param offset 相对于origin指定位置的偏移量
 * @param origin 相对于哪个位置计算偏移量
 * @return
 */
xfat_err_t xfile_seek(xfile_t * file, xfile_ssize_t offset, xfile_orgin_t origin) {
    xfat_err_t err = FS_ERR_OK;
    xfile_ssize_t final_pos;
    xfile_size_t offset_to_move;
    u32_t curr_cluster, curr_pos;

    // 获取最终的定位位置
    switch (origin) {
    case XFAT_SEEK_SET:
        final_pos = offset;
        break;
    case XFAT_SEEK_CUR:
        final_pos = file->pos + offset;
        break;
    case XFAT_SEEK_END:
        final_pos = file->size + offset;
        break;
    default:
        final_pos = -1;
        break;            
    }

    // 超出文件范围
    if ((final_pos < 0) || (final_pos >= file->size)) {
        return FS_ERR_PARAM;
    }

    // 相对于当前要调整的偏移量
    offset = final_pos - file->pos;
    if (offset > 0) {
        curr_cluster = file->curr_cluster;
        curr_pos = file->pos;
        offset_to_move = (xfile_size_t)offset;
    } else {
        curr_cluster = file->start_cluster;
        curr_pos = 0;
        offset_to_move = (xfile_size_t)final_pos;
    }

    while (offset_to_move > 0) {
        u32_t cluster_offset = to_cluster_offset(file->xfat, curr_pos);
        xfile_size_t curr_move = offset_to_move;

        // 不超过当前簇
        if (cluster_offset + curr_move < file->xfat->cluster_byte_size) {
            curr_pos += curr_move;
            break;
        }

        // 超过当前簇，只在当前簇内移动
        curr_move = file->xfat->cluster_byte_size - cluster_offset;
        curr_pos += curr_move;
        offset_to_move -= curr_move;

        // 进入下一簇: 是否要判断后续簇是否正确？
        err = get_next_cluster(file->xfat, curr_cluster, &curr_cluster);
        if (err < 0) {
            file->err = err;
            return err;
        }
    } 

    file->pos = curr_pos;
    file->curr_cluster = curr_cluster;
    return FS_ERR_OK;
}

/**
 * 关闭已经打开的文件
 * @param file 待关闭的文件
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
