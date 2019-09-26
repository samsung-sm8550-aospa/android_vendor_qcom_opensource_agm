/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#define LOGTAG "AGM: graph"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include "gsl_intf.h"
#include "graph.h"
#include "graph_module.h"
#include "metadata.h"
#include "utils.h"

#define DEVICE_RX 0
#define DEVICE_TX 1

/* TODO: remove this later after including in gecko header files */
#define PARAM_ID_SOFT_PAUSE_START 0x800102e
#define PARAM_ID_SOFT_PAUSE_RESUME 0x800102f

#define CONVX(x) #x
#define CONV_TO_STRING(x) CONVX(x)

#define ADD_MODULE(x, y) \
                 ({ \
                   module_info_t *add_mod = NULL;\
                   add_mod = calloc(1, sizeof(module_info_t));\
                   if (add_mod != NULL) {\
                       *add_mod = x;\
                       if (y != NULL) add_mod->dev_obj = y; \
                       list_add_tail(&graph_obj->tagged_mod_list, &add_mod->list);\
                   } \
                   add_mod;\
                 })

typedef struct module_info_link_list {
   module_info_t *data;
   struct listnode tagged_list;
}module_info_link_list_t;


static int get_acdb_files_from_directory(const char* acdb_files_path,
                                         struct gsl_acdb_data_files *data_files)
{
    int result = 0;
    int ret = 0;
    int i = 0;
    DIR *dir_fp = NULL;
    struct dirent *dentry;

    dir_fp = opendir(acdb_files_path);
    if (dir_fp == NULL) {
        AGM_LOGE("cannot open acdb path %s\n", acdb_files_path);
        ret = EINVAL;
        goto done;
    }

    /* search directory for .acdb files */
    while ((dentry = readdir(dir_fp)) != NULL) {
        if ((strstr(dentry->d_name, ".acdb") != NULL)) {
           if (data_files->num_files >= GSL_MAX_NUM_OF_ACDB_FILES) {
               AGM_LOGE("Reached max num of files, %d!\n", i);
               break;
           }
           result = snprintf(
                    data_files->acdbFiles[i].fileName,
                    sizeof(data_files->acdbFiles[i].fileName),
                    "%s/%s", acdb_files_path, dentry->d_name);
           if ((result < 0) ||
               (result >= (int)sizeof(data_files->acdbFiles[i].fileName))) {
               AGM_LOGE("snprintf failed: %s/%s, err %d\n",
                              acdb_files_path,
                              data_files->acdbFiles[i].fileName,
                              result);
               ret = -EINVAL;
               break;
           }
           data_files->acdbFiles[i].fileNameLen =
                                    (uint32_t)strlen(data_files->acdbFiles[i].fileName);
           AGM_LOGI("Load file: %s\n", data_files->acdbFiles[i].fileName);
           i++;
        }
    }

    if (i == 0)
        AGM_LOGE("No .acdb files found in %s!\n", acdb_files_path);

    data_files->num_files = i;

    closedir(dir_fp);
done:
    return ret;
}

int configure_buffer_params(struct graph_obj *gph_obj,
                            struct session_obj *sess_obj)
{
    struct gsl_cmd_configure_read_write_params buf_config = {0};
    int ret = 0;
    size_t size = 0;
    enum gsl_cmd_id cmd_id;
    enum agm_data_mode mode = sess_obj->stream_config.data_mode;

    AGM_LOGD("%s sess buf_sz %d num_bufs %d", sess_obj->stream_config.dir == RX?
                 "Playback":"Capture", sess_obj->buffer_config.size,
                  sess_obj->buffer_config.count);

    buf_config.buff_size = (uint32_t)sess_obj->buffer_config.size;
    buf_config.num_buffs = sess_obj->buffer_config.count;
    buf_config.start_threshold = sess_obj->stream_config.start_threshold;
    buf_config.stop_threshold = sess_obj->stream_config.stop_threshold;
    buf_config.shmem_ep_tag = SHMEM_ENDPOINT;
    /**
     *TODO:expose a flag to chose between different data passing modes
     *BLOCKING/NON-BLOCKING/SHARED_MEM.
     */
    if (mode == AGM_DATA_BLOCKING)
        buf_config.attributes = GSL_DATA_MODE_BLOCKING;
    else if (mode == AGM_DATA_NON_BLOCKING)
        buf_config.attributes = GSL_DATA_MODE_NON_BLOCKING;
    else {
        AGM_LOGE("Unsupported buffer mode : %d, Default to Blocking", mode);
        buf_config.attributes = GSL_DATA_MODE_BLOCKING;
    }

    size = sizeof(struct gsl_cmd_configure_read_write_params);

    if (sess_obj->stream_config.dir == RX)
       cmd_id = GSL_CMD_CONFIGURE_WRITE_PARAMS;
    else
       cmd_id = GSL_CMD_CONFIGURE_READ_PARAMS;

    ret = gsl_ioctl(gph_obj->graph_handle, cmd_id, &buf_config, size);

    if (ret != 0) {
        AGM_LOGE("Buffer configuration failed error %d", ret);
    } else {
       gph_obj->buf_config  = buf_config;
    }
    AGM_LOGD("exit");
    return ret;
}

int graph_init()
{
    uint32_t ret = 0;
    struct gsl_acdb_data_files acdb_files;
    struct gsl_acdb_file delta_file;
    struct gsl_init_data init_data;
    const char *delta_file_path;

    /*Populate acdbfiles from the shared file path*/
    acdb_files.num_files = 0;

#ifdef ACDB_PATH
    ret = get_acdb_files_from_directory(CONV_TO_STRING(ACDB_PATH), &acdb_files);
    if (ret != 0)
       return ret;
#else
#  error "Define -DACDB_PATH="PATH" in the makefile to compile"
#endif

#ifdef ACDB_DELTA_FILE_PATH
    delta_file_path = CONV_TO_STRING(ACDB_DELTA_FILE_PATH);
    if ((strlen(delta_file_path) + 1) > sizeof(delta_file.fileName)) {
       AGM_LOGE("path is big than what gsl handles");
       ret = -EINVAL;
       return ret;
    }
    delta_file.fileNameLen = strlen(delta_file_path) + 1;
    ret = strlcpy(delta_file.fileName, delta_file_path, delta_file.fileNameLen);
#else
#  error "Define -DACDB_DELTA_FILE_PATH="PATH" in the makefile to compile"
#endif

    init_data.acdb_files = &acdb_files;
    init_data.acdb_delta_file = &delta_file;
    init_data.acdb_addr = 0x0;
    init_data.max_num_ready_checks = 1;
    init_data.ready_check_interval_ms = 100;
    ret = gsl_init(&init_data);
    if (ret != 0) {
        AGM_LOGE("gsl_init failed error %d \n", ret);
        goto deinit_gsl;
    }
    return 0;

deinit_gsl:
    return ret;
}

int graph_deinit()
{

    gsl_deinit();
    return 0;
}

void gsl_callback_func(struct gsl_event_cb_params *event_params,
                       void *client_data)
{
     struct graph_obj *graph_obj = (struct graph_obj *) client_data;

     if (graph_obj == NULL) {
         AGM_LOGE("Invalid graph object");
         return;
     }
     if (event_params == NULL) {
         AGM_LOGE("event params NULL");
         return;
     }

     if (graph_obj->cb)
         graph_obj->cb((struct agm_event_cb_params *)event_params,
                        graph_obj->client_data);

     return;
}

int graph_get_tags_with_module_info(struct agm_key_vector_gsl *gkv,
				    void *payload, size_t *size)
{
    return gsl_get_tags_with_module_info((struct gsl_key_vector *) gkv,
                                         payload, size);
}

static int get_tags_with_module_info(struct agm_key_vector_gsl *gkv,
                                    void **payload, size_t *size)
{
    int ret = 0;

    ret = gsl_get_tags_with_module_info((struct gsl_key_vector *)gkv, NULL, size);
    if (ret != 0) {
        AGM_LOGI("cannot get tags size info\n");
        goto done;
    }

    *payload = calloc(1, *size);
    if (!payload) {
        AGM_LOGE("Not enough memory for payload\n");
        ret = -ENOMEM;
        goto done;
    }

    ret = gsl_get_tags_with_module_info((struct gsl_key_vector *)gkv, *payload,
                                         size);
    if (ret != 0)
        AGM_LOGI("Failed to  get tags info with error no %d\n",ret);
done:
    return ret;
}

static int add_to_list(uint32_t module_list_count, module_info_t *info, struct listnode *node)
{
    uint32_t count = 0;
    int ret = 0;
    module_info_link_list_t *mod_list = NULL;

    for (count = 0; count < module_list_count; count++) {
        mod_list = calloc(1, sizeof(module_info_link_list_t));
        if (!mod_list) {
            AGM_LOGE("Not enough memory for payload\n");
            ret = -ENOMEM;
            goto done;
        }
        mod_list->data = info + count;
        list_add_tail(node, &mod_list->tagged_list);
    }
done:
    return ret;
}

int graph_open(struct agm_meta_data_gsl *meta_data_kv,
               struct session_obj *sess_obj, struct device_obj *dev_obj,
               struct graph_obj **gph_obj)
{
    struct graph_obj *graph_obj = NULL;
    int ret = 0;
    struct listnode *temp_node, *node = NULL, *node_list = NULL;
    size_t module_info_size;
    struct gsl_tag_module_info *tag_module_info = NULL;
    size_t tag_module_info_size;
    struct gsl_tag_module_info_entry *gsl_tag_entry = NULL;
    struct agm_key_vector_gsl *gkv;
    int i = 0, j = 0;
    size_t module_list_count =  0;
    module_info_t *mod, *temp_mod = NULL;
    module_info_link_list_t *mod_list = NULL;
    size_t arraysize = 0;
    module_info_t *stream_module_list = NULL;
    module_info_t *hw_ep_module = NULL;

    list_declare(node_sess);
    list_init(&node_sess);
    list_declare(node_hw);
    list_init(&node_hw);


    AGM_LOGD("entry");
    if (meta_data_kv == NULL || gph_obj == NULL) {
        AGM_LOGE("Invalid input\n");
        ret = -EINVAL;
        goto done;
    }

    graph_obj = calloc (1, sizeof(struct graph_obj));
    if (graph_obj == NULL) {
        AGM_LOGE("failed to allocate graph object");
        ret = -ENOMEM;
        goto done;
    }
    list_init(&graph_obj->tagged_mod_list);
    pthread_mutex_init(&graph_obj->lock, (const pthread_mutexattr_t *)NULL);

    /**
     *TODO:In the current config parameters we dont have a
     *way to know if a hostless session (loopback)
     *is session loopback or device loopback
     *We assume now that if it is a hostless
     *session then it is device loopback always.
     *And hence we configure pcm decoder/encoder/convertor
     *only in case of a no hostless session.
     */

    /*Get all the tags info of the graph and store it tag_module_info structure*/
    ret = get_tags_with_module_info(&meta_data_kv->gkv,
                                      &tag_module_info, &tag_module_info_size);
    if (ret != 0)
        goto free_graph_obj;

    gsl_tag_entry = (struct gsl_tag_module_info_entry *)(tag_module_info->tag_module_entry);

    /* Add all default stream_module to node_sess list */
    get_stream_module_list_array(&stream_module_list, &arraysize);
    module_list_count =  arraysize /sizeof(struct module_info);
    ret = add_to_list(module_list_count, stream_module_list, &node_sess);
    if (ret != 0)
        goto free_graph_obj;

    /* Add all default hw_ep_module to node_hw list */
    get_hw_ep_module_list_array(&hw_ep_module, &arraysize);
    module_list_count =  arraysize /sizeof(struct module_info);
    ret = add_to_list(module_list_count, hw_ep_module, &node_hw);
    if (ret != 0)
        goto free_graph_obj;

    for (i = 0; i < tag_module_info->num_tags; i++) {
        if (sess_obj != NULL) {
            /**
             * Compare each stream_module present in node_sess list with
             * gsl_tag_entry and if match found we are adding it to the graph
             * module_list using ADD_MODULE . And remove this module from
             * node_sess list . So that in next iterationit will not compare the
             * same module with gsl_tag_entry again.
             */
            list_for_each(node_list, &node_sess) {
                mod_list = node_to_item(node_list, module_info_link_list_t, tagged_list);
                if (gsl_tag_entry->tag_id == mod_list->data->tag) {
                    if (gsl_tag_entry->num_modules > 1) {
                        AGM_LOGD("modules num  is invalid");
                        goto free_graph_obj;
                    }
                    mod = mod_list->data;
                    mod->miid = gsl_tag_entry->module_entry[0].module_iid;
                    mod->mid = gsl_tag_entry->module_entry[0].module_id;
                    AGM_LOGD("miid %x mid %x tag %x", mod->miid, mod->mid, mod->tag);
                    ADD_MODULE(*mod, NULL);
                    /*Remove the module from the node_sess list*/
                    list_remove(node_list);
                    goto tag_list;
                }
            }
        }

        if (dev_obj != NULL) {
            /**
             * Compare each hw_ep_module present in node_hw list with
             * gsl_tag_entry and if match found we are adding it to the graph
             * module_list using ADD_MODULE . And remove this module from
             * node_hw list.So that in next iteration it will not compare the
             * same module with gsl_tag_entry again.
             */
            list_for_each(node_list, &node_hw) {
                mod_list = node_to_item(node_list, module_info_link_list_t, tagged_list);
                if (gsl_tag_entry->tag_id == mod_list->data->tag) {
                    if (gsl_tag_entry->num_modules > 1) {
                        AGM_LOGD("modules num  is invalid");
                        goto free_graph_obj;
                    }
                    mod = mod_list->data;
                    mod->miid = gsl_tag_entry->module_entry[0].module_iid;
                    mod->mid = gsl_tag_entry->module_entry[0].module_id;
                    /*store GKV which describes/contains this module*/
                    gkv = calloc(1, sizeof(struct agm_key_vector_gsl));
                    if (!gkv) {
                        AGM_LOGE("No memory to create merged metadata\n");
                        ret = -ENOMEM;
                        goto free_graph_obj;
                    }
                    gkv->num_kvs = meta_data_kv->gkv.num_kvs;
                    gkv->kv = calloc(gkv->num_kvs, sizeof(struct agm_key_value));
                    if (!gkv->kv) {
                        AGM_LOGE("No memory to create merged metadata gkv\n");
                        free(gkv);
                        ret = -ENOMEM;
                        goto free_graph_obj;
                    }
                    memcpy(gkv->kv, meta_data_kv->gkv.kv,
                          gkv->num_kvs * sizeof(struct agm_key_value));
                    mod->gkv = gkv;
                    AGM_LOGD("miid %x mid %x tag %x", mod->miid, mod->mid, mod->tag);
                    ADD_MODULE(*mod, dev_obj);
                    /*Remove the module from node_hw list*/
                    list_remove(node_list);
                    goto tag_list;
                }
            }
        }
tag_list:
        gsl_tag_entry  = (char *)gsl_tag_entry + sizeof(gsl_tag_entry) +
                               (sizeof(struct gsl_module_id_info_entry) *
                               gsl_tag_entry->num_modules);
    }
    graph_obj->sess_obj = sess_obj;

    ret = gsl_open((struct gsl_key_vector *)&meta_data_kv->gkv,
                   (struct gsl_key_vector *)&meta_data_kv->ckv,
                   &graph_obj->graph_handle);
    if (ret != 0) {
       AGM_LOGE("Failed to open the graph with error %d", ret);
       goto free_graph_obj;
    }

    ret = gsl_register_event_cb(graph_obj->graph_handle,
                                gsl_callback_func, graph_obj);
    if (ret != 0) {
        AGM_LOGE("failed to register callback");
        goto close_graph;
    }
    graph_obj->state = OPENED;
    *gph_obj = graph_obj;

    goto done;

close_graph:
    gsl_close(graph_obj->graph_handle);
free_graph_obj:
    /*free the list of modules associated with this graph_object*/
    list_for_each_safe(node, temp_node, &graph_obj->tagged_mod_list) {
        list_remove(node);
        temp_mod = node_to_item(node, module_info_t, list);
        if (temp_mod->gkv) {
            free(temp_mod->gkv->kv);
            free(temp_mod->gkv);
        }
        free(temp_mod);
    }
    pthread_mutex_destroy(&graph_obj->lock);
    free(graph_obj);
done:
    if (tag_module_info)
        free(tag_module_info);
    return ret;
}

int graph_close(struct graph_obj *graph_obj)
{
    int ret = 0;
    struct listnode *temp_node,*node = NULL;
    module_info_t *temp_mod = NULL;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }
    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry handle %x", graph_obj->graph_handle);

    ret = gsl_close(graph_obj->graph_handle);
    if (ret !=0)
        AGM_LOGE("gsl close failed error %d", ret);
    /*free the list of modules associated with this graph_object*/
    list_for_each_safe(node, temp_node, &graph_obj->tagged_mod_list) {
        list_remove(node);
        temp_mod = node_to_item(node, module_info_t, list);
        if (temp_mod->gkv) {
            free(temp_mod->gkv->kv);
            free(temp_mod->gkv);
        }
        free(temp_mod);
    }
    pthread_mutex_unlock(&graph_obj->lock);
    pthread_mutex_destroy(&graph_obj->lock);
    free(graph_obj);
    AGM_LOGD("exit");
    return ret;
}

int graph_prepare(struct graph_obj *graph_obj)
{
    int ret = 0;
    struct listnode *node = NULL;
    module_info_t *mod = NULL;
    struct session_obj *sess_obj = NULL;
    struct agm_session_config stream_config;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }
    sess_obj = graph_obj->sess_obj;

    if (sess_obj == NULL) {
        AGM_LOGE("invalid sess object");
        return -EINVAL;
    }
    stream_config = sess_obj->stream_config;

    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);
    pthread_mutex_lock(&graph_obj->lock);
    /**
     *Iterate over mod list to configure each module
     *present in the graph. Also validate if the module list
     *matches the configuration passed by the client.
     */
    list_for_each(node, &graph_obj->tagged_mod_list) {
        mod = node_to_item(node, module_info_t, list);
        if (mod->is_configured)
            continue;
        if ((mod->tag == STREAM_INPUT_MEDIA_FORMAT) && stream_config.is_hostless) {
            AGM_LOGE("Shared mem mod present for a hostless session error out");
            ret = -EINVAL;
            goto done;
        }

        if (((mod->tag == STREAM_PCM_DECODER) &&
              (stream_config.dir == TX)) ||
            ((mod->tag == STREAM_PCM_ENCODER) &&
              (stream_config.dir == RX))) {
            AGM_LOGE("Session cfg (dir = %d) does not match session module %x",
                      stream_config.dir, mod->module);
             ret = -EINVAL;
             goto done;
        }

        if ((mod->dev_obj != NULL) && (mod->dev_obj->refcnt.start == 0)) {
            if (((mod->tag == DEVICE_HW_ENDPOINT_RX) &&
                (mod->dev_obj->hw_ep_info.dir == AUDIO_INPUT)) ||
               ((mod->tag == DEVICE_HW_ENDPOINT_TX) &&
                (mod->dev_obj->hw_ep_info.dir == AUDIO_OUTPUT))) {
               AGM_LOGE("device cfg (dir = %d) does not match dev module %x",
                         mod->dev_obj->hw_ep_info.dir, mod->module);
               ret = -EINVAL;
               goto done;
            }
        }
        if (mod->configure) {
            ret = mod->configure(mod, graph_obj);
            if (ret != 0)
                goto done;
            mod->is_configured = true;
        }
    }

    /*Configure buffers only if it is not a hostless session*/
    if (sess_obj != NULL && !stream_config.is_hostless) {
        ret = configure_buffer_params(graph_obj, sess_obj);
        if (ret != 0) {
            AGM_LOGE("buffer configuration failed \n");
            goto done;
        }
    }

    ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_PREPARE, NULL, 0);
    if (ret !=0) {
        AGM_LOGE("graph_prepare failed %d", ret);
        goto done;
    }
    graph_obj->state = PREPARED;

done:
    pthread_mutex_unlock(&graph_obj->lock);
    AGM_LOGD("exit");
    return ret;
}

int graph_start(struct graph_obj *graph_obj)
{
    int ret = 0;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);
    if (!(graph_obj->state & (PREPARED | STOPPED))) {
       AGM_LOGE("graph object is not in correct state, current state %d",
                    graph_obj->state);
       ret = -EINVAL;
       goto done;
    }
    ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_START, NULL, 0);
    if (ret !=0) {
        AGM_LOGE("graph_start failed %d", ret);
        goto done;
    }
    graph_obj->state = STARTED;

done:
    pthread_mutex_unlock(&graph_obj->lock);
    AGM_LOGD("exit");
    return ret;
}

int graph_stop(struct graph_obj *graph_obj,
               struct agm_meta_data_gsl *meta_data)
{
    int ret = 0;
    struct gsl_cmd_properties gsl_cmd_prop = {0};

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);
    if (!(graph_obj->state & (STARTED))) {
       AGM_LOGE("graph object is not in correct state, current state %d",
                    graph_obj->state);
       ret = -EINVAL;
       goto done;
    }

    if (meta_data) {
        memcpy (&(gsl_cmd_prop.gkv), &(meta_data->gkv),
                                       sizeof(struct gsl_key_vector));
        gsl_cmd_prop.property_id = meta_data->sg_props.prop_id;
        gsl_cmd_prop.num_property_values = meta_data->sg_props.num_values;
        gsl_cmd_prop.property_values = meta_data->sg_props.values;

        ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_STOP,
                        &gsl_cmd_prop, sizeof(struct gsl_cmd_properties));
    } else {
        ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_STOP, NULL, 0);
    }
    if (ret !=0) {
        AGM_LOGE("graph stop failed %d", ret);
        goto done;
    }

    graph_obj->state = STOPPED;

done:
    pthread_mutex_unlock(&graph_obj->lock);
    AGM_LOGD("exit");
    return ret;
}

int graph_pause_resume(struct graph_obj *graph_obj, bool pause)
{
    int ret = 0;
    struct listnode *node = NULL;
    module_info_t *mod;
    struct apm_module_param_data_t *header;
    size_t payload_size = 0;
    uint8_t *payload = NULL;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    /* Pause module info is retrived and added to list in graph_open */
    list_for_each(node, &graph_obj->tagged_mod_list) {
        mod = node_to_item(node, module_info_t, list);
        if (mod->tag == TAG_PAUSE) {
            AGM_LOGD("Soft Pause module IID 0x%x, Pause: %d", mod->miid, pause);

            payload_size = sizeof(struct apm_module_param_data_t);
            ALIGN_PAYLOAD(payload_size, 8);

            payload = calloc(1, (size_t)payload_size);
            if (!payload) {
                AGM_LOGE("No memory to allocate for payload");
                ret = -ENOMEM;
                goto done;
            }

            header = (struct apm_module_param_data_t*)payload;
            header->module_instance_id = mod->miid;
            if (pause)
                header->param_id = PARAM_ID_SOFT_PAUSE_START;
            else
                header->param_id = PARAM_ID_SOFT_PAUSE_RESUME;

            header->error_code = 0x0;
            header->param_size = 0x0;

            pthread_mutex_lock(&graph_obj->lock);
            ret = gsl_set_custom_config(graph_obj->graph_handle, payload, payload_size);
            if (ret !=0)
                AGM_LOGE("graph_set_custom_config failed %d", ret);
            pthread_mutex_unlock(&graph_obj->lock);
            free(payload);
            break;
        }
    }

done:
    return ret;
}


int graph_pause(struct graph_obj *graph_obj)
{
    return graph_pause_resume(graph_obj, true);
}

int graph_resume(struct graph_obj *graph_obj)
{
    return graph_pause_resume(graph_obj, false);
}

int graph_set_config(struct graph_obj *graph_obj, void *payload,
                     size_t payload_size)
{
    int ret = 0;
    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);
    ret = gsl_set_custom_config(graph_obj->graph_handle, payload, payload_size);
    if (ret !=0)
        AGM_LOGE("%s: graph_set_config failed %d", __func__, ret);

    pthread_mutex_unlock(&graph_obj->lock);

    return ret;
}

int graph_get_config(struct graph_obj *graph_obj, void *payload,
                     size_t payload_size)
{
    int ret = 0;
    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %p", graph_obj->graph_handle);
    ret = gsl_get_custom_config(graph_obj->graph_handle, payload, payload_size);
    if (ret !=0)
        AGM_LOGE("%s: graph_get_config failed %d", __func__, ret);

    pthread_mutex_unlock(&graph_obj->lock);

    return ret;
}

int graph_set_config_with_tag(struct graph_obj *graph_obj,
                              struct agm_key_vector_gsl *gkv,
                              struct agm_tag_config_gsl *tag_config)
{
     int ret = 0;

     if (graph_obj == NULL) {
         AGM_LOGE("invalid graph object");
         return -EINVAL;
     }

     pthread_mutex_lock(&graph_obj->lock);
     ret = gsl_set_config(graph_obj->graph_handle, (struct gsl_key_vector *)gkv,
                          tag_config->tag_id, (struct gsl_key_vector *)&tag_config->tkv);
     if (ret)
         AGM_LOGE("graph_set_config failed %d", ret);

     pthread_mutex_unlock(&graph_obj->lock);

     return ret;
}

int graph_set_cal(struct graph_obj *graph_obj,
                  struct agm_meta_data_gsl *metadata)
{
     int ret = 0;

     if (graph_obj == NULL) {
         AGM_LOGE("invalid graph object");
         return -EINVAL;
     }

     pthread_mutex_lock(&graph_obj->lock);
     ret = gsl_set_cal(graph_obj->graph_handle,
                       (struct gsl_key_vector *)&metadata->gkv,
                       (struct gsl_key_vector *)&metadata->ckv);
     if (ret)
         AGM_LOGE("graph_set_cal failed %d", ret);

     pthread_mutex_unlock(&graph_obj->lock);

     return ret;
}

int graph_write(struct graph_obj *graph_obj, void *buffer, size_t *size)
{
    int ret = 0;
    struct gsl_buff gsl_buff;
    uint32_t size_written = 0;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }
    pthread_mutex_lock(&graph_obj->lock);
    // TODO: update below check
   /* if (!(graph_obj->state & (PREPARED|STARTED))) {
        AGM_LOGE("Cannot add a graph in start state");
        ret = -EINVAL;
        goto done;
    }*/

    /*TODO: Update the write api to take timeStamps/other buffer meta data*/
    gsl_buff.timestamp = 0x0;
    /*TODO: get the FLAG info from client e.g. FLAG_EOS)*/
    gsl_buff.flags = 0;
    gsl_buff.size = *size;
    gsl_buff.addr = (uint8_t *)(buffer);
    ret = gsl_write(graph_obj->graph_handle,
                    SHMEM_ENDPOINT, &gsl_buff, &size_written);
    if (ret != 0) {
        AGM_LOGE("gsl_write for size %d failed with error %d", size, ret);
        goto done;
    }
    *size = size_written;
done:
    pthread_mutex_unlock(&graph_obj->lock);

    return ret;
}

int graph_read(struct graph_obj *graph_obj, void *buffer, size_t *size)
{
    int ret = 0;
    struct gsl_buff gsl_buff;
    int size_read = 0;
    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }
    pthread_mutex_lock(&graph_obj->lock);
    if (!(graph_obj->state & STARTED)) {
        AGM_LOGE("Cannot add a graph in start state");
        ret = -EINVAL;
        goto done;
    }

    /*TODO: Update the write api to take timeStamps/other buffer meta data*/
    gsl_buff.timestamp = 0x0;
    /*TODO: get the FLAG info from client e.g. FLAG_EOS)*/
    gsl_buff.flags = 0;
    gsl_buff.size = *size;
    gsl_buff.addr = (uint8_t *)(buffer);
    ret = gsl_read(graph_obj->graph_handle,
                    SHMEM_ENDPOINT, &gsl_buff, &size_read);
    if ((ret != 0) || (size_read == 0)) {
        AGM_LOGE("size_requested %d size_read %d error %d",
                      size, size_read, ret);
    }
    *size = size_read;
done:
    pthread_mutex_unlock(&graph_obj->lock);
    return ret;
}

int graph_add(struct graph_obj *graph_obj,
              struct agm_meta_data_gsl *meta_data_kv,
              struct device_obj *dev_obj)
{
    int ret = 0;
    struct session_obj *sess_obj;
    struct gsl_cmd_graph_select add_graph;
    module_info_t *mod = NULL;
    struct agm_key_vector_gsl *gkv;
    struct listnode *node = NULL;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);

    if (graph_obj->state < OPENED) {
        AGM_LOGE("Cannot add a graph in %d state", graph_obj->state);
        ret = -EINVAL;
        goto done;
    }
    /**
     *This api is used to add a new device leg ( is device object is
     *present in the argument or to add an already exisiting graph.
     *Hence we need not add any new session (stream) related modules
     */

    /*Add the new GKV to the current graph*/
    add_graph.graph_key_vector.num_kvps = meta_data_kv->gkv.num_kvs;
    add_graph.graph_key_vector.kvp = (struct gsl_key_value_pair *)
                                     meta_data_kv->gkv.kv;
    add_graph.cal_key_vect.num_kvps = meta_data_kv->ckv.num_kvs;
    add_graph.cal_key_vect.kvp = (struct gsl_key_value_pair *)
                                     meta_data_kv->ckv.kv;
    ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_ADD_GRAPH, &add_graph,
                    sizeof(struct gsl_cmd_graph_select));
    if (ret != 0) {
        AGM_LOGE("graph add failed with error %d", ret);
        goto done;
    }
    if (dev_obj != NULL) {
        module_info_t *add_module, *temp_mod = NULL;
        size_t module_info_size;
        struct gsl_module_id_info *module_info;
        bool mod_present = false;
        size_t arraysize;
        module_info_t *hw_ep_module = NULL;
        get_hw_ep_module_list_array(&hw_ep_module, &arraysize);
        if (dev_obj->hw_ep_info.dir == AUDIO_OUTPUT)
            mod = &hw_ep_module[0];
        else
            mod = &hw_ep_module[1];

        ret = gsl_get_tagged_module_info((struct gsl_key_vector *)
                                           &meta_data_kv->gkv,
                                           mod->tag,
                                           &module_info, &module_info_size);
        if (ret != 0) {
            AGM_LOGE("cannot get tagged module info for module %x",
                          mod->tag);
            ret = -EINVAL;
            goto done;
        }
        mod->miid = module_info->module_entry[0].module_iid;
        mod->mid = module_info->module_entry[0].module_id;
        /**
         *Check if this is the same device object as was passed for graph open
         *or a new one.We do this by comparing the module_iid of the module
         *present in the graph object with the one returned from the above api.
         *if it is a new module we add it to the list and configure it.
         */
        list_for_each(node, &graph_obj->tagged_mod_list) {
            temp_mod = node_to_item(node, module_info_t, list);
            if (temp_mod->miid == mod->miid) {
                mod_present = true;
                break;
            }
        }
        if (!mod_present) {
            /**
             *This is a new device object, add this module to the list and
             */
            /*Make a local copy of gkv and use when we query gsl
            for tagged data*/
            gkv = calloc(1, sizeof(struct agm_key_vector_gsl));
            if (!gkv) {
                AGM_LOGE("No memory to allocate for gkv\n");
                ret = -ENOMEM;
                goto done;
            }
            gkv->num_kvs = meta_data_kv->gkv.num_kvs;
            gkv->kv = calloc(gkv->num_kvs, sizeof(struct agm_key_value));
            if (!gkv->kv) {
                AGM_LOGE("No memory to allocate for kv\n");
                free(gkv);
                ret = -ENOMEM;
                goto done;
            }
            memcpy(gkv->kv, meta_data_kv->gkv.kv,
                    gkv->num_kvs * sizeof(struct agm_key_value));
            mod->gkv = gkv;
            gkv = NULL;
            AGM_LOGD("Adding the new module tag %x mid %x miid %x", mod->tag, mod->mid, mod->miid);
            ADD_MODULE(*mod, dev_obj);
        }
    }
    /*configure the newly added modules*/
    list_for_each(node, &graph_obj->tagged_mod_list) {
        mod = node_to_item(node, module_info_t, list);
        /* Need to configure SPR module again for the new device */
        if (mod->is_configured && !(mod->tag == TAG_STREAM_SPR))
            continue;
        if (mod->configure) {
            ret = mod->configure(mod, graph_obj);
            if (ret != 0)
                goto done;
            mod->is_configured = true;
        }
    }
done:
    pthread_mutex_unlock(&graph_obj->lock);
    AGM_LOGD("exit");
    return ret;
}

int graph_change(struct graph_obj *graph_obj,
                     struct agm_meta_data_gsl *meta_data_kv,
                     struct device_obj *dev_obj)
{
    int ret = 0;
    struct session_obj *sess_obj;
    struct gsl_cmd_graph_select change_graph;
    module_info_t *mod = NULL;
    struct agm_key_vector_gsl *gkv;
    struct listnode *node, *temp_node = NULL;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);
    if (graph_obj->state & STARTED) {
        AGM_LOGE("Cannot change graph in start state");
        ret = -EINVAL;
        goto done;
    }

    /**
     *GSL closes the old graph if CHANGE_GRAPH command is issued.
     *Hence reset is_configured to ensure that all the modules are
     *configured once again.
     *
     */
    list_for_each(node, &graph_obj->tagged_mod_list) {
        mod = node_to_item(node, module_info_t, list);
        mod->is_configured = false;
    }

    if (dev_obj != NULL) {
        mod = NULL;
        module_info_t *add_module, *temp_mod = NULL;
        size_t module_info_size;
        struct gsl_module_id_info *module_info;
        bool mod_present = false;
        size_t arraysize;
        module_info_t *hw_ep_module = NULL;
        get_hw_ep_module_list_array(&hw_ep_module, &arraysize);
        if (dev_obj->hw_ep_info.dir == AUDIO_OUTPUT)
            mod = &hw_ep_module[0];
        else
            mod = &hw_ep_module[1];

        ret = gsl_get_tagged_module_info((struct gsl_key_vector *)
                                           &meta_data_kv->gkv,
                                           mod->tag,
                                           &module_info, &module_info_size);
        if (ret != 0) {
            AGM_LOGE("cannot get tagged module info for module %x",
                          mod->tag);
            ret = -EINVAL;
            goto done;
        }
        /**
         *Check if this is the same device object as was passed for graph open
         *or a new one.We do this by comparing the module_iid of the module
         *present in the graph object with the one returned from the above api.
         *If this is a new module, we delete the older device tagged module
         *as it is not part of the graph anymore (would have been removed as a
         *part of graph_remove).
         */
        list_for_each(node, &graph_obj->tagged_mod_list) {
            temp_mod = node_to_item(node, module_info_t, list);
            if (temp_mod->miid == module_info->module_entry[0].module_iid) {
                mod_present = true;
                break;
            }
        }
        if (!mod_present) {
            /*This is a new device object, add this module to the list and
             *delete the current hw_ep(Device module) from the list.
             */
            list_for_each_safe(node, temp_node, &graph_obj->tagged_mod_list) {
                temp_mod = node_to_item(node, module_info_t, list);
                if ((temp_mod->tag == DEVICE_HW_ENDPOINT_TX) ||
                    (temp_mod->tag == DEVICE_HW_ENDPOINT_RX)) {
                    list_remove(node);
                    if (temp_mod->gkv) {
                        free(temp_mod->gkv->kv);
                        free(temp_mod->gkv);
                    }
                    free(temp_mod);
                    temp_mod = NULL;
                }
            }
            add_module = ADD_MODULE(*mod, dev_obj);
            if (!add_module) {
                AGM_LOGE("No memory to allocate for add_module\n");
                ret = -ENOMEM;
                goto done;
            }
            add_module->miid = module_info->module_entry[0].module_iid;
            add_module->mid = module_info->module_entry[0].module_id;
            /*Make a local copy of gkv and use when we query gsl
            for tagged data*/
            gkv = calloc(1, sizeof(struct agm_key_vector_gsl));
            if (!gkv) {
                AGM_LOGE("No memory to allocate for gkv\n");
                ret = -ENOMEM;
                goto done;
            }
            gkv->num_kvs = meta_data_kv->gkv.num_kvs;
            gkv->kv = calloc(gkv->num_kvs, sizeof(struct agm_key_value));
            if (!gkv->kv) {
                AGM_LOGE("No memory to allocate for kv\n");
                free(gkv);
                ret = -ENOMEM;
                goto done;
            }
            memcpy(gkv->kv, meta_data_kv->gkv.kv,
                    gkv->num_kvs * sizeof(struct agm_key_value));
            add_module->gkv = gkv;
            gkv = NULL;
        }
    }
    /*Send the new GKV for CHANGE_GRAPH*/
    change_graph.graph_key_vector.num_kvps = meta_data_kv->gkv.num_kvs;
    change_graph.graph_key_vector.kvp = (struct gsl_key_value_pair *)
                                     meta_data_kv->gkv.kv;
    change_graph.cal_key_vect.num_kvps = meta_data_kv->ckv.num_kvs;
    change_graph.cal_key_vect.kvp = (struct gsl_key_value_pair *)
                                     meta_data_kv->ckv.kv;
    ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_CHANGE_GRAPH, &change_graph,
                    sizeof(struct gsl_cmd_graph_select));
    if (ret != 0) {
        AGM_LOGE("graph add failed with error %d", ret);
        goto done;
    }
    /*configure modules again*/
    list_for_each(node, &graph_obj->tagged_mod_list) {
        mod = node_to_item(node, module_info_t, list);
        if (mod->configure) {
            ret = mod->configure(mod, graph_obj);
            if (ret != 0)
                goto done;
            mod->is_configured = true;
        }
    }
done:
    pthread_mutex_unlock(&graph_obj->lock);
    AGM_LOGD("exit");
    return ret;
}

int graph_remove(struct graph_obj *graph_obj,
                 struct agm_meta_data_gsl *meta_data_kv)
{
    int ret = 0;
    struct gsl_cmd_remove_graph rm_graph;

    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }
    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %x", graph_obj->graph_handle);

    /**
     *graph_remove would only pass the graph which needs to be removed.
     *to GSL. Once graph remove is done, sesison obj will have to issue
     *graph_change/graph_add if reconfiguration of modules is needed, otherwise
     *graph_start will suffice.graph remove wont reconfigure the modules.
     */
    rm_graph.graph_key_vector.num_kvps = meta_data_kv->gkv.num_kvs;
    rm_graph.graph_key_vector.kvp = (struct gsl_key_value_pair *)
                                     meta_data_kv->gkv.kv;
    ret = gsl_ioctl(graph_obj->graph_handle, GSL_CMD_REMOVE_GRAPH, &rm_graph,
                    sizeof(struct gsl_cmd_remove_graph));
    if (ret != 0) {
        AGM_LOGE("graph add failed with error %d", ret);
    }

    pthread_mutex_unlock(&graph_obj->lock);
    AGM_LOGD("exit");
    return ret;
}

int graph_register_cb(struct graph_obj *gph_obj, event_cb cb,
                      void *client_data)
{

    if (gph_obj == NULL){
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }

    if (cb == NULL) {
        AGM_LOGE("No callback to register");
        return -EINVAL;
    }

    pthread_mutex_lock(&gph_obj->lock);
    gph_obj->cb = cb;
    gph_obj->client_data = client_data;
    pthread_mutex_unlock(&gph_obj->lock);

    return 0;
}

int graph_register_for_events(struct graph_obj *gph_obj,
                              struct agm_event_reg_cfg *evt_reg_cfg)
{
    int ret = 0;
    struct gsl_cmd_register_custom_event *reg_ev_payload = NULL;
    size_t payload_size = 0;

    if (gph_obj == NULL){
        AGM_LOGE("invalid graph object");
        ret = -EINVAL;
        goto done;
    }

    if (evt_reg_cfg == NULL) {
        AGM_LOGE("No event register payload passed");
        ret = -EINVAL;
        goto done;
    }
    pthread_mutex_lock(&gph_obj->lock);

    if (gph_obj->graph_handle == NULL) {
        pthread_mutex_unlock(&gph_obj->lock);
        AGM_LOGE("invalid graph handle");
        ret = -EINVAL;
        goto done;
    }
    payload_size = sizeof(struct gsl_cmd_register_custom_event) +
                                       evt_reg_cfg->event_config_payload_size;

    reg_ev_payload = calloc(1, payload_size);
    if (reg_ev_payload == NULL) {
        pthread_mutex_unlock(&gph_obj->lock);
        AGM_LOGE("calloc failed for reg_ev_payload");
        ret = -ENOMEM;
        goto done;
    }

    reg_ev_payload->event_id = evt_reg_cfg->event_id;
    reg_ev_payload->module_instance_id = evt_reg_cfg->module_instance_id;
    reg_ev_payload->event_config_payload_size =
                                 evt_reg_cfg->event_config_payload_size;
    reg_ev_payload->is_register = evt_reg_cfg->is_register;

    memcpy(reg_ev_payload + sizeof(apm_module_register_events_t),
          evt_reg_cfg->event_config_payload, evt_reg_cfg->event_config_payload_size);

    ret = gsl_ioctl(gph_obj->graph_handle, GSL_CMD_REGISTER_CUSTOM_EVENT, reg_ev_payload, payload_size);
    if (ret != 0) {
       AGM_LOGE("event registration failed with error %d", ret);
    }
    pthread_mutex_unlock(&gph_obj->lock);

done:
    return ret;
}

size_t graph_get_hw_processed_buff_cnt(struct graph_obj *graph_obj,
                                       enum direction dir)
{
    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object or null callback");
        return 0;
    }
    /*TODO: Uncomment that call once platform moves to latest GSL release*/
    return 2 /*gsl_get_processed_buff_cnt(graph_obj->graph_handle, dir)*/;

}

int graph_eos(struct graph_obj *graph_obj)
{
    if (graph_obj == NULL) {
        AGM_LOGE("invalid graph object");
        return -EINVAL;
    }
        AGM_LOGE("enter");
    return gsl_ioctl(graph_obj->graph_handle, GSL_CMD_EOS, NULL, 0);
}

int graph_get_session_time(struct graph_obj *graph_obj, uint64_t *tstamp)
{
    int ret = 0;
    uint8_t *payload = NULL;
    struct apm_module_param_data_t *header;
    struct param_id_spr_session_time_t *sess_time;
    size_t payload_size = 0;
    uint64_t timestamp;

    if (graph_obj == NULL || tstamp == NULL) {
        AGM_LOGE("Invalid Input Params");
        return -EINVAL;
    }

    pthread_mutex_lock(&graph_obj->lock);
    AGM_LOGD("entry graph_handle %p", graph_obj->graph_handle);
    if (!(graph_obj->state & (STARTED))) {
       AGM_LOGE("graph object is not in correct state, current state %d",
                    graph_obj->state);
       ret = -EINVAL;
       goto done;
    }
    if (graph_obj->spr_miid == 0) {
        AGM_LOGE("Invalid SPR module IID to query timestamp");
        goto done;
    }
    AGM_LOGV("SPR module IID: %x", graph_obj->spr_miid);

    payload_size = sizeof(struct apm_module_param_data_t) +
        sizeof(struct param_id_spr_session_time_t);
    /*ensure that the payloadsize is byte multiple */
    ALIGN_PAYLOAD(payload_size, 8);

    payload = calloc(1, (size_t)payload_size);
    if (!payload)
        goto done;

    header = (struct apm_module_param_data_t*)payload;
    sess_time = (struct param_id_spr_session_time_t *)
                     (payload + sizeof(struct apm_module_param_data_t));

    header->module_instance_id = graph_obj->spr_miid;
    header->param_id = PARAM_ID_SPR_SESSION_TIME;
    header->error_code = 0x0;
    header->param_size = payload_size;

    ret = gsl_get_custom_config(graph_obj->graph_handle, payload, payload_size);
    if (ret != 0) {
        AGM_LOGE("gsl_get_custom_config command failed with error %d", ret);
        goto get_fail;
    }
    AGM_LOGV("session_time: msw[%ld], lsw[%ld], at: msw[%ld], lsw[%ld] ts: msw[%ld], lsw[%ld]\n",
              sess_time->session_time.value_msw,
              sess_time->session_time.value_lsw,
              sess_time->absolute_time.value_msw,
              sess_time->absolute_time.value_lsw,
              sess_time->timestamp.value_msw,
              sess_time->timestamp.value_lsw);

    timestamp = (uint64_t)sess_time->session_time.value_msw;
    timestamp = timestamp  << 32 | sess_time->session_time.value_lsw;
    *tstamp = timestamp;

get_fail:
    free(payload);
done:
    pthread_mutex_unlock(&graph_obj->lock);
    return ret;
}