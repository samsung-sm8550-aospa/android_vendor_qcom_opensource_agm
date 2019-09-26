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
#define LOGTAG "AGM: session"

#include <malloc.h>
#include "session_obj.h"
#include "utils.h"

#define GSL_EVENT_SRC_MODULE_ID_GSL 0x2001 // DO NOT CHANGE

//forward declarations
static int session_close(struct session_obj *sess_obj);
static int session_set_loopback(struct session_obj *sess_obj, uint32_t session_id, bool enable);

static struct aif *aif_obj_get_from_pool(struct session_obj *sess_obj, uint32_t aif)
{
	struct listnode *node;
	struct aif *aif_node;

	list_for_each(node, &sess_obj->aif_pool) {
		aif_node = node_to_item(node, struct aif, node);
		if (aif_node->aif_id == aif)
			return aif_node;
	}

	return NULL;
}

static struct aif* aif_obj_create(struct session_obj *sess_obj, int aif_id)
{
	struct aif *aif_obj = NULL;
	struct device_obj *dev_obj = NULL;
	int ret = 0;

	aif_obj = calloc(1, sizeof(struct aif));
	if (!aif_obj) {
		AGM_LOGE("%s: Memory allocation failed for aif object\n", __func__);
		return aif_obj;
	}

	ret = device_get_obj(aif_id, &dev_obj);
	if (ret || !dev_obj) {
		AGM_LOGE("%s: Error:%d retrieving device object with id:%d \n", __func__, ret, aif_obj->aif_id);
		goto done;
	}
	aif_obj->aif_id = aif_id;
	aif_obj->dev_obj = dev_obj;

done:
	return aif_obj;
}

/* returns aif_obj associated with aif id in the session obj */
int aif_obj_get(struct session_obj *sess_obj, int aif_id, struct aif **aif_obj)
{
	// return from list, if not there create, add to list and return
	struct aif *tobj = NULL;
	int ret = 0;

	tobj = aif_obj_get_from_pool(sess_obj, aif_id);
	if (!tobj) {
		//AGM_LOGE("%s: Couldnt find a aif object in the list, creating one\n", __func__);

		tobj = aif_obj_create(sess_obj, aif_id);
		if (!tobj) {
			AGM_LOGE("%s: Couldnt create an aif object\n", __func__);
			ret = -ENOMEM;
			return ret;
		}
		list_add_tail(&sess_obj->aif_pool, &tobj->node);
	}

	*aif_obj = tobj;

	return ret;
}

/* returns aif_obj associated with aif id in the session obj with state not as specified in the argument */
uint32_t aif_obj_get_count_with_state(struct session_obj *sess_obj, enum aif_state state, bool exact_state_match)
{
	uint32_t count = 0;
	struct listnode *node;
	struct aif *temp = NULL;

	//check how many devices in connected state
	list_for_each(node, &sess_obj->aif_pool) {
		temp = node_to_item(node, struct aif, node);
		if (!temp) {
			AGM_LOGE("%s Error could not find aif node\n", __func__);
			continue;
		}

		if ((exact_state_match  && temp->state == state) ||
			(!exact_state_match && temp->state >= state)) {
			count++;
		}
	}

	return count;
}

static struct agm_meta_data_gsl* session_get_merged_metadata(struct session_obj *sess_obj)
{
	struct agm_meta_data_gsl *merged = NULL;
	struct agm_meta_data_gsl *temp = NULL;
	struct listnode *node;
	struct aif *aif_node;

	list_for_each(node, &sess_obj->aif_pool) {
		aif_node = node_to_item(node, struct aif, node);
		merged = metadata_merge(4, temp, &sess_obj->sess_meta, &aif_node->sess_aif_meta, &aif_node->dev_obj->metadata);
		if (temp) {
			metadata_free(temp);
			free(temp);
		}
		temp = merged;
	}

	return merged;
}

static struct agm_meta_data_gsl* session_get_merged_metadata_without_aif(struct session_obj *sess_obj)
{
	struct agm_meta_data_gsl *merged = NULL;
	struct agm_meta_data_gsl *temp = NULL;
	struct listnode *node;
	struct aif *aif_node;

	list_for_each(node, &sess_obj->aif_pool) {
		aif_node = node_to_item(node, struct aif, node);
		merged = metadata_merge(3, temp, &sess_obj->sess_meta, &aif_node->sess_aif_meta);
		if (temp) {
			metadata_free(temp);
			free(temp);
		}
		temp = merged;
	}

	return merged;
}

static int session_pool_init()
{
	int ret = 0;
	sess_pool = calloc(1, sizeof(struct session_pool));
	if (!sess_pool) {
		AGM_LOGE("%s: No Memory to create sess_pool\n", __func__);
		ret = -ENOMEM;
		goto done;
	}
	list_init(&sess_pool->session_list);
	pthread_mutex_init(&sess_pool->lock, (const pthread_mutexattr_t *) NULL);

done:
	return ret;
}

static void aif_free(struct aif *aif_obj)
{
	metadata_free(&aif_obj->sess_aif_meta);
	free(aif_obj->params);
	free(aif_obj);
}

static void aif_pool_free(struct session_obj *sess_obj)
{
	struct aif *aif_obj;
	struct listnode *node, *next;

	list_for_each_safe(node, next, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		list_remove(&aif_obj->node);
		aif_free(aif_obj);
	}
}

static void session_cb_pool_free(struct session_obj *sess_obj)
{
	struct session_cb *sess_cb;
	struct listnode *node, *next;

	list_for_each_safe(node, next, &sess_obj->cb_pool) {
		sess_cb = node_to_item(node, struct session_cb, node);
		list_remove(&sess_cb->node);
		free(sess_cb);
	}

}

static void sess_obj_free(struct session_obj *sess_obj)
{
	aif_pool_free(sess_obj);
	session_cb_pool_free(sess_obj);
	metadata_free(&sess_obj->sess_meta);
	free(sess_obj->params);
	free(sess_obj);
}

static void session_pool_free()
{
	struct session_obj *sess_obj;
	struct listnode *node, *next;
	int ret = 0;

	pthread_mutex_lock(&sess_pool->lock);
	list_for_each_safe(node, next, &sess_pool->session_list) {
		sess_obj = node_to_item(node, struct session_obj, node);
		pthread_mutex_lock(&sess_obj->lock);
		ret = session_close(sess_obj);
		if (ret) {
			AGM_LOGE("%s, Error:%d closing session with session id:%d\n", __func__, ret, sess_obj->sess_id);
		}
		pthread_mutex_unlock(&sess_obj->lock);

		//cleanup aif pool from session_object
		list_remove(&sess_obj->node);
		sess_obj_free(sess_obj);
	}
	pthread_mutex_unlock(&sess_pool->lock);
	free(sess_pool);
}

static struct session_obj* session_obj_create(int session_id)
{
	struct session_obj *obj = NULL;

	obj = calloc(1, sizeof(struct session_obj));
	if (!obj) {
		AGM_LOGE("%s: Memory allocation failed for sesssion object\n", __func__);
		return obj;
	}

	obj->sess_id = session_id;
	list_init(&obj->aif_pool);
	list_init(&obj->cb_pool);
	pthread_mutex_init(&obj->lock, (const pthread_mutexattr_t *) NULL);
	pthread_mutex_init(&obj->cb_pool_lock, (const pthread_mutexattr_t *) NULL);

	return obj;
}

struct session_obj *session_obj_retrieve_from_pool(uint32_t session_id)
{
	struct session_obj *obj = NULL;
	struct listnode *node;

	pthread_mutex_lock(&sess_pool->lock);
	list_for_each(node, &sess_pool->session_list) {
		obj = node_to_item(node, struct session_obj, node);
		if (obj->sess_id == session_id)
			break;
		else
			obj = NULL;
	}
	pthread_mutex_unlock(&sess_pool->lock);

	return obj;
}

struct session_obj *session_obj_get_from_pool(uint32_t session_id)
{
	struct session_obj *obj = NULL;
	struct listnode *node;

	pthread_mutex_lock(&sess_pool->lock);
	list_for_each(node, &sess_pool->session_list) {
		obj = node_to_item(node, struct session_obj, node);
		if (obj->sess_id == session_id)
			break;
		else
			obj = NULL;
	}

	if (!obj) {
		//AGM_LOGE("%s: Couldnt find a session object in the list, creating one\n",  __func__);
		obj = session_obj_create(session_id);
		if (!obj) {
			AGM_LOGE("%s: Couldnt create a session object\n",  __func__);
			goto done;
		}
		list_add_tail(&sess_pool->session_list, &obj->node);
	}

done:
	pthread_mutex_unlock(&sess_pool->lock);
	return obj;
}

/* returns session_obj associated with session id */
int session_obj_get(int session_id, struct session_obj **obj)
{
	// return from list, if not there create, add to list and return
	struct session_obj *tobj = NULL;
	int ret = 0;

	tobj = session_obj_get_from_pool(session_id);
	if (!tobj) {
		AGM_LOGE("%s, Couldnt find or create session_obj\n", __func__);
		ret = -ENOMEM;
	}

	*obj = tobj;
	return ret;
}

static int session_set_loopback(struct session_obj *sess_obj, uint32_t pb_id, bool enable)
{
	int ret = 0;
	struct session_obj *pb_obj = NULL;
	struct agm_meta_data_gsl *capture_metadata = NULL;
	struct agm_meta_data_gsl *playback_metadata = NULL;
	struct agm_meta_data_gsl *merged_metadata = NULL;

	/*
	 * 1. merged metadata of pb session + cap session
	 * 2. call graph_add
	 * 3. call start (prepare doesnt achieve anything so skip)
	 * 4. Expectation for loopback is that its establishing an edge b/w TX and RX session
	 *    and no new subgraphs are added and hence no gsl_start/prepare.
	 *    So no new modules/subgraphs which require configuration is expected and hence
	 *    no separate setparams() for loopback for now.
	 */
	ret = session_obj_get(pb_id, &pb_obj);
	if (ret) {
		AGM_LOGE("%s: Error:%d getting session object with session id:%d\n", __func__, ret, pb_id);
		goto done;
	}

	capture_metadata = session_get_merged_metadata(sess_obj);
	if (!capture_metadata) {
		ret = -ENOMEM;
		AGM_LOGE("%s: Error:%d, merging metadata with session id=%d\n", __func__, ret, sess_obj->sess_id);
		goto done;
	}

	playback_metadata = session_get_merged_metadata(pb_obj);
	if (!playback_metadata) {
		ret = -ENOMEM;
		AGM_LOGE("%s: Error:%d, merging metadata with session id=%d\n", __func__, ret, pb_id);
		goto done;
	}

	merged_metadata = metadata_merge(2, capture_metadata, playback_metadata);
	if (!merged_metadata) {
		ret = -ENOMEM;
		AGM_LOGE("%s: Error:%d, merging metadata with playback session id=%d and capture session id=%d\n",
				__func__, ret, pb_id, sess_obj->sess_id);
		goto done;
	}

	if (enable)
		ret = graph_add(sess_obj->graph, merged_metadata, NULL);
	else
		ret = graph_remove(sess_obj->graph, merged_metadata);

	if (ret) {
		AGM_LOGE("%s: Error:%d graph %s failed for session_id: %d\n",
			__func__, ret, ((enable == true) ? "add":"remove"), sess_obj->sess_id);
		goto done;
	}

done:
	if (capture_metadata) {
		metadata_free(capture_metadata);
		free(capture_metadata);
	}
	if (playback_metadata) {
		metadata_free(playback_metadata);
		free(playback_metadata);
	}
	if (merged_metadata) {
		metadata_free(merged_metadata);
		free(merged_metadata);
	}
	return ret;
}

static int session_set_ec_ref(struct session_obj *sess_obj, uint32_t aif_id, bool enable)
{
	int ret = 0;
	struct agm_meta_data_gsl *capture_metadata = NULL;
	struct agm_meta_data_gsl *merged_metadata = NULL;
	struct device_obj *dev_obj = NULL;


	ret = device_get_obj(aif_id, &dev_obj);
	if (ret) {
		AGM_LOGE("%s: Error:%d, unable to get dev_obj with aif_id=%d\n", __func__, ret, aif_id);
		goto done;
	}

	capture_metadata = session_get_merged_metadata_without_aif(sess_obj);
	if (!capture_metadata) {
		ret = -ENOMEM;
		AGM_LOGE("%s: Error:%d, merging metadata with session id=%d\n", __func__, ret, sess_obj->sess_id);
		goto done;
	}

	merged_metadata = metadata_merge(2, capture_metadata, dev_obj->metadata);
	if (!merged_metadata) {
		ret = -ENOMEM;
		AGM_LOGE("%s: Error:%d, merging metadata with capture session id=%d aif_id:%d \n",
				__func__, ret, sess_obj->sess_id, aif_id);
		goto done;
	}

	if (enable)
		ret = graph_add(sess_obj->graph, merged_metadata, NULL);
	else
		ret = graph_remove(sess_obj->graph, merged_metadata);

	if (ret) {
		AGM_LOGE("%s: Error:%d graph %s failed for session_id: %d\n",
			__func__, ret, ((enable == true) ? "add":"remove"), sess_obj->sess_id);
		goto done;
	}

done:
	if (capture_metadata) {
		metadata_free(capture_metadata);
		free(capture_metadata);
	}
	if (merged_metadata) {
		metadata_free(merged_metadata);
		free(merged_metadata);
	}
	return ret;
}

static int session_disconnect_aif(struct session_obj *sess_obj, struct aif *aif_obj, uint32_t opened_count)
{
	int ret = 0;
	struct agm_meta_data_gsl *merged_metadata = NULL;
	struct agm_meta_data_gsl *merged_meta_sess_aif = NULL;
	struct agm_meta_data_gsl temp = {0};
	struct graph_obj *graph = sess_obj->graph;

	merged_metadata = metadata_merge(3, &sess_obj->sess_meta, &aif_obj->sess_aif_meta, &aif_obj->dev_obj->metadata);
	if (!merged_metadata) {
		AGM_LOGE("%s: No memory to create merged_metadata session_id: %d, audio interface id:%d \n",
						__func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -ENOMEM;
		return ret;
	}

	if (opened_count == 1) {
		//this is SSSD condition, hence stop just the stream/stream-device, merged only sess-aif, aif
		merged_meta_sess_aif = metadata_merge(2, &aif_obj->sess_aif_meta, &aif_obj->dev_obj->metadata);
		if (!merged_meta_sess_aif) {
			AGM_LOGE("%s: No memory to create merged_metadata session_id: %d, audio interface id:%d \n",
							__func__, sess_obj->sess_id, aif_obj->aif_id);
			ret = -ENOMEM;
			return ret;
		}

		temp.gkv = merged_metadata->gkv;
		temp.ckv = merged_metadata->ckv;
		temp.sg_props = merged_meta_sess_aif->sg_props;

		ret = graph_stop(graph, &temp);
		if (ret) {
			AGM_LOGE("%s: Error:%d graph stop failed session_id: %d, audio interface id:%d \n",
				__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
		}
	} else {
		ret = graph_remove(graph, merged_metadata);
		if (ret) {
			AGM_LOGE("%s: Error:%d graph remove failed session_id: %d, audio interface id:%d \n",
				__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
		}
	}

	ret = device_close(aif_obj->dev_obj);
	if (ret) {
		AGM_LOGE("%s: Error:%d closing device object with id:%d \n",
			__func__, ret, aif_obj->aif_id);
	}

	metadata_free(merged_metadata);
	return ret;
}

static void graph_event_cb(struct agm_event_cb_params *event_params,
                         void *client_data)
{
	struct session_obj *sess_obj = NULL;
	struct session_cb *sess_cb;
	struct listnode *node, *next;
	uint32_t session_id = (uint32_t)((uintptr_t)client_data);

	if (!event_params) {
		AGM_LOGE("%s: event_parms is NULL", __func__);
		return;
	}

	sess_obj = session_obj_retrieve_from_pool(session_id);
	if (!sess_obj) {
		AGM_LOGE("%s: Incorrect client_data:%d, doesn't match sess_obj from pool",
			__func__, session_id);
		return;
	}

	pthread_mutex_lock(&sess_obj->cb_pool_lock);
	list_for_each_safe(node, next, &sess_obj->cb_pool) {
		sess_cb = node_to_item(node, struct session_cb, node);
		if (sess_cb && sess_cb->cb) {
			/* Filter callbacks based on event_id and event_type */
			if (sess_cb->evt_type == AGM_EVENT_DATA_PATH &&
				event_params->source_module_id == GSL_EVENT_SRC_MODULE_ID_GSL &&
				(event_params->event_id == AGM_EVENT_EOS_RENDERED ||
				event_params->event_id == AGM_EVENT_READ_DONE ||
				event_params->event_id == AGM_EVENT_WRITE_DONE)) {
				    sess_cb->cb(sess_obj->sess_id, (struct agm_event_cb_params *)event_params, sess_cb->client_data);
			} else if (sess_cb->evt_type == AGM_EVENT_MODULE &&
					   event_params->source_module_id != GSL_EVENT_SRC_MODULE_ID_GSL) {
				sess_cb->cb(sess_obj->sess_id, (struct agm_event_cb_params *)event_params, sess_cb->client_data);
			}
		}
	}
	pthread_mutex_unlock(&sess_obj->cb_pool_lock);
}

static int session_connect_aif(struct session_obj *sess_obj, struct aif *aif_obj, uint32_t opened_count)
{
	int ret = 0;
	struct agm_meta_data_gsl *merged_metadata = NULL;
	struct graph_obj *graph = sess_obj->graph;

	//step 2.a  merge metadata
	merged_metadata = metadata_merge(3, &sess_obj->sess_meta, &aif_obj->sess_aif_meta, &aif_obj->dev_obj->metadata);
	if (!merged_metadata) {
		AGM_LOGE("%s: Error merging metadata session_id:%d aif_id:%d\n",
			__func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -ENOMEM;
		goto done;
	}

	ret = device_open(aif_obj->dev_obj);
	if (ret) {
		AGM_LOGE("%s: Error:%d opening device object with id:%d \n",
			__func__, ret, aif_obj->aif_id);
		goto done;
	}

	//step 2.b
	if (opened_count == 0) {
		if (sess_obj->state == SESSION_CLOSED) {
			ret = graph_open(merged_metadata, sess_obj, aif_obj->dev_obj, &sess_obj->graph);
			graph = sess_obj->graph;
			if (ret) {
				AGM_LOGE("%s: Error:%d graph open failed session_id: %d, audio interface id:%d \n",
					__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
				goto close_device;
			}

			//register callback
			ret = graph_register_cb(graph, graph_event_cb, (void *)((uintptr_t) sess_obj->sess_id));
			if (ret) {
				AGM_LOGE("%s: Error:%d graph callback registration failed session_id: %d\n",
					__func__, ret, sess_obj->sess_id);
				goto graph_cleanup;
			}
		} else {
			ret = graph_change(graph, merged_metadata, aif_obj->dev_obj);
			if (ret) {
				AGM_LOGE("%s: Error:%d graph change failed session_id: %d, audio interface id:%d \n",
					__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
				goto close_device;
			}
		}
	} else {
			ret = graph_add(graph, merged_metadata, aif_obj->dev_obj);
			if (ret) {
				AGM_LOGE("%s: Error:%d graph add failed session_id: %d, audio interface id:%d \n",
					__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
				goto close_device;
			}
	}

	//step 2.c set cached params for stream only in closed
	if (sess_obj->state == SESSION_CLOSED && sess_obj->params != NULL) {
		ret = graph_set_config(graph, sess_obj->params, sess_obj->params_size);
		if (ret) {
			AGM_LOGE("%s: Error:%d setting session cached params: %d\n",
				__func__, ret, sess_obj->sess_id);
			goto graph_cleanup;
		}
	}

	//step 2.c set cached device(streamdevice + device) params
	if (aif_obj->params != NULL) {
		ret = graph_set_config(graph, aif_obj->params, aif_obj->params_size);
		if (ret) {
			AGM_LOGE("%s: Error:%d setting session cached params: %d\n",
				__func__, ret, sess_obj->sess_id);
			goto graph_cleanup;
		}
	}

	goto done;

graph_cleanup:
	if (opened_count == 0)
		graph_close(sess_obj->graph);
	else 
		graph_remove(sess_obj->graph, merged_metadata);

close_device:
	device_close(aif_obj->dev_obj);

done:
	if (merged_metadata) {
		metadata_free(merged_metadata);
		free(merged_metadata);
	}

	return ret;
}

static int session_open_with_first_device(struct session_obj *sess_obj)
{
	int ret = 0;
	struct aif *aif_obj = NULL, *temp = NULL;
	struct listnode *node;

	list_for_each(node, &sess_obj->aif_pool) {
		temp = node_to_item(node, struct aif, node);
		if (temp->state == AIF_OPEN) {
			aif_obj = temp;
			break;
		}
	}

	if (!aif_obj) {
		AGM_LOGE("%s: No Audio interface(Backend) set on session(Frontend):%d\n",
				__func__, sess_obj->sess_id);
		ret = -EPIPE;
		goto done;
	}

	ret = session_connect_aif(sess_obj, aif_obj, 0);
	if (ret) {
		AGM_LOGE("%s: Audio interface(Backend):%d <-> session(Frontend):%d Connect failed error:%d\n",
				__func__,  aif_obj->aif_id, sess_obj->sess_id, ret);
		goto done;
	}
	aif_obj->state = AIF_OPENED;

done:
	return ret;
}

static int session_connect_reminder_devices(struct session_obj *sess_obj)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	struct listnode *node;
	uint32_t opened_count = 0;

	// opened_count is 1 because this function is being called after connecting with 1 device
	opened_count = 1;

	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (aif_obj && aif_obj->state == AIF_OPEN) {
			ret = session_connect_aif(sess_obj, aif_obj, opened_count);
			if (ret) {
				AGM_LOGE("%s: Audio interface(Backend): %d <-> session(Frontend): %d Connect failed error:%d\n",
						__func__, aif_obj->aif_id, sess_obj->sess_id, ret);
				goto unwind;
			}

			aif_obj->state = AIF_OPENED;
			opened_count++;
		}
	}

	return 0;

unwind:
	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (aif_obj && aif_obj->state == AIF_OPENED) {
			/*TODO: fix the 3rd argument to provide correct count*/
			ret = session_disconnect_aif(sess_obj, aif_obj, 1);
			if (ret) {
				AGM_LOGE("%s: Error:%d initializing session_pool\n", __func__, ret);
			}
			aif_obj->state = AIF_OPEN;
			opened_count--;
		}
	}
	ret = graph_close(sess_obj->graph);
	if (ret) {
		AGM_LOGE("%s: Error:%d initializing session_pool\n", __func__, ret);
	}
	sess_obj->graph = NULL;

	return ret;
}

static int session_prepare(struct session_obj *sess_obj)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	enum direction dir = sess_obj->stream_config.dir;
	struct listnode *node = NULL;
	uint32_t count = 0;

	count = aif_obj_get_count_with_state(sess_obj, AIF_OPENED, false);
	if (count == 0) {
		AGM_LOGE("%s Error:%d No aif in right state to proceed with session start for sessionid :%d\n", __func__, ret, sess_obj->sess_id);
		return -1; //-EINVALID;
	}

	if (dir == TX) {
		ret = graph_prepare(sess_obj->graph);
		if (ret) {
			AGM_LOGE("%s Error:%d preparing graph\n", __func__, ret);
			goto done;
		}
	}

	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (!aif_obj) {
			AGM_LOGE("%s Error:%d could not find aif node\n", __func__, ret);
			goto done;
		}

		//TODO 1: in device switch cases, only the aif not prepared should be prepared.
		if (aif_obj->state == AIF_OPENED || aif_obj->state == AIF_STOPPED) {
			ret = device_prepare(aif_obj->dev_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d preparing device\n", __func__, ret);
				goto done;
			}
			aif_obj->state = AIF_PREPARED;
		}
	}

	if (dir == RX) {
		ret = graph_prepare(sess_obj->graph);
		if (ret) {
			AGM_LOGE("%s Error:%d preparing graph\n", __func__, ret);
			goto done;
		}
	}

	sess_obj->state = SESSION_PREPARED;
	return ret;

done:
	return ret;
}

static int session_start(struct session_obj *sess_obj)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	enum direction dir = sess_obj->stream_config.dir;
	struct listnode *node = NULL;
	uint32_t count = 0;
	struct session_obj *pb_obj = NULL;
	struct device_obj *ec_ref_dev_obj = NULL;

	count = aif_obj_get_count_with_state(sess_obj, AIF_OPENED, false);
	if (count == 0) {
		AGM_LOGE("%s Error:%d No aif in right state to proceed with session start for session id :%d\n",
				__func__, ret, sess_obj->sess_id);
		return -EINVAL;
	}

	if (dir == TX) {

		// For loopback, check if the playback session is in STARTED state, otherwise return failure
		if (sess_obj->loopback_state == true) {
			ret = session_obj_get(sess_obj->loopback_sess_id, &pb_obj);
			if (ret) {
				AGM_LOGE("%s: Error:%d getting session object with session id:%d\n",
						__func__, ret, sess_obj->loopback_sess_id);
				goto done;
			}

			if (pb_obj->state != SESSION_STARTED) {
				AGM_LOGE("%s: Error:%d Playback session with session id:%d\n not in STARTED state, current state:%d\n",
						__func__, ret, pb_obj->sess_id, pb_obj->state);
				ret = -EINVAL;
				goto done;
			}
		}

		// For ec ref, check if the device object is in STARTED, otherwise return failure
		if (sess_obj->ec_ref_state == true) {
			ret = device_get_obj(sess_obj->ec_ref_aif_id, &ec_ref_dev_obj);
			if (ret) {
				AGM_LOGE("%s: Error:%d getting device object with aif id:%d\n",
						__func__, ret, sess_obj->ec_ref_aif_id);
				goto done;
			}

			if (ec_ref_dev_obj->state != DEV_STARTED) {
				AGM_LOGE("%s: Error:%d Device object with aif id:%d\n not in STARTED state, current state:%d\n",
						__func__, ret, sess_obj->ec_ref_aif_id,
						ec_ref_dev_obj->state);
				ret = -EINVAL;
				goto done;
			}

		}

		ret = graph_start(sess_obj->graph);
		if (ret) {
			AGM_LOGE("%s Error:%d starting graph\n", __func__, ret);
			goto done;
		}
	}

	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (!aif_obj) {
			AGM_LOGE("%s Error:%d could not find aif node\n", __func__, ret);
			goto unwind;
		}

		if (aif_obj->state == AIF_OPENED || aif_obj->state == AIF_PREPARED || aif_obj->state == AIF_STOPPED ) {
			ret = device_start(aif_obj->dev_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d starting device id:%d\n", __func__, ret, aif_obj->aif_id);
				goto unwind;
			}
			aif_obj->state = AIF_STARTED;
		}
	}

	if (dir == RX) {
		ret = graph_start(sess_obj->graph);
		if (ret) {
			AGM_LOGE("%s Error:%d starting graph\n", __func__, ret);
			goto unwind;
		}
	}

	sess_obj->state = SESSION_STARTED;
	return ret;

unwind:

	if (dir == TX)
		graph_stop(sess_obj->graph, NULL);

	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (aif_obj && (aif_obj->state == AIF_STARTED)) {
			device_stop(aif_obj->dev_obj);
			//If start fails, client will retry with a prepare call, so moving to opened state will allow prepare to go through
			aif_obj->state = AIF_OPENED;
		}
	}

done:
	return ret;
}

static int session_stop(struct session_obj *sess_obj)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	enum direction dir = sess_obj->stream_config.dir;
	struct listnode *node = NULL;

	if (sess_obj->state != SESSION_STARTED) {
		AGM_LOGE("%s session not in STARTED state, current state:%d\n", __func__, sess_obj->state);
		return -EINVAL;
	}

	if (dir == RX) {
		ret = graph_stop(sess_obj->graph, NULL);
		if (ret) {
			AGM_LOGE("%s Error:%d starting graph\n", __func__, ret);
			goto done;
		}
	}

	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (!aif_obj) {
			AGM_LOGE("%s Error:%d could not find aif node\n", __func__, ret);
			continue;
		}

		if (aif_obj->state == AIF_STARTED) {
			ret = device_stop(aif_obj->dev_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d stopping device id:%d\n", __func__, ret, aif_obj->aif_id);

			}
			aif_obj->state = AIF_STOPPED;
		}
	}

	if (dir == TX) {
		ret = graph_stop(sess_obj->graph, NULL);
		if (ret) {
			AGM_LOGE("%s Error:%d stopping graph\n", __func__, ret);
		}
	}

	sess_obj->state = SESSION_STOPPED;
	return ret;

done:
	return ret;
}

static int session_close(struct session_obj *sess_obj)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	struct listnode *node = NULL;

	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s session already in CLOSED state\n", __func__);
		return -EALREADY;
	}

	if (sess_obj->state == SESSION_STARTED) {
		ret = graph_stop (sess_obj->graph, NULL);
        if (ret) {
		   AGM_LOGE("%s Error:%d closing graph\n", __func__, ret);
	    }
	}

	ret = graph_close(sess_obj->graph);
	if (ret) {
		AGM_LOGE("%s Error:%d closing graph\n", __func__, ret);
	}
	sess_obj->graph = NULL;

	list_for_each(node, &sess_obj->aif_pool) {
		aif_obj = node_to_item(node, struct aif, node);
		if (!aif_obj) {
			AGM_LOGE("%s Error:%d could not find aif node\n", __func__, ret);
			continue;
		}

		if (aif_obj->state >= AIF_OPENED) {
			ret = device_close(aif_obj->dev_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d stopping device id:%d\n", __func__, ret, aif_obj->aif_id);
			}
			aif_obj->state = AIF_CLOSED;
		}
	}

	sess_obj->state = SESSION_CLOSED;
        AGM_LOGE("exit");
	return ret;
}


int session_obj_deinit()
{
	session_pool_free();
	device_deinit();
	graph_deinit();
	return 0;
}

/* Initializes session_obj, enumerate and fill session related information */
int session_obj_init()
{
	int ret = 0;

	ret = device_init();
	if (ret) {
		AGM_LOGE("%s: Error:%d initializing device\n", __func__, ret);
		goto done;
	}

	ret = graph_init();
	if (ret) {
		AGM_LOGE("%s: Error:%d initializing graph\n", __func__, ret);
		goto device_deinit;
	}

	ret = session_pool_init();
	if (ret) {
		AGM_LOGE("%s: Error:%d initializing session_pool\n", __func__, ret);
		goto graph_deinit;
	}
	goto done;

graph_deinit:
	graph_deinit();

device_deinit:
	device_deinit();

done:
	return ret;
}

int session_obj_set_sess_metadata(struct session_obj *sess_obj, uint32_t size, uint8_t *metadata)
{

	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	ret = metadata_copy(&(sess_obj->sess_meta), size, metadata);
	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}

int session_obj_set_sess_params(struct session_obj *sess_obj,
	void *payload, size_t size)
{
   int ret = 0;

   pthread_mutex_lock(&sess_obj->lock);

   if (sess_obj->params) {
	   free(sess_obj->params);
	   sess_obj->params = NULL;
	   sess_obj->params_size = 0;
   }

   sess_obj->params = calloc(1, size);
   if (!sess_obj->params) {
	   AGM_LOGE("%s: No memory for sess params on sess_id:%d\n", __func__, sess_obj->sess_id);
	   ret = -EINVAL;
	   goto done;
   }

   memcpy(sess_obj->params, payload, size);
   sess_obj->params_size = size;

   if (sess_obj->state != SESSION_CLOSED) {
	   ret = graph_set_config(sess_obj->graph, sess_obj->params, sess_obj->params_size);
	   if (ret) {
		   AGM_LOGE("%s:Error:%d setting for sess params on sess_id:%d\n",
				   __func__, ret, sess_obj->sess_id);
		   free(sess_obj->params);
		   sess_obj->params = NULL;
		   sess_obj->params_size = 0;
	   }
   }

done:
   pthread_mutex_unlock(&sess_obj->lock);
   return ret;
}

int session_obj_set_sess_aif_params(struct session_obj *sess_obj,
	uint32_t aif_id,
	void* payload, size_t size)
{
	int ret = 0;
	struct aif *aif_obj = NULL;

	pthread_mutex_lock(&sess_obj->lock);
	ret = aif_obj_get(sess_obj, aif_id, &aif_obj);
	if (ret) {
		AGM_LOGE("%s: Error obtaining aif object with sess_id:%d,  aif id:%d\n",
			__func__, sess_obj->sess_id, aif_id);
		goto done;
	}

   if (aif_obj->params) {
	   free(aif_obj->params);
	   aif_obj->params = NULL;
	   aif_obj->params_size = 0;
   }

   aif_obj->params = calloc(1, size);
   if (!aif_obj->params) {
	   AGM_LOGE("%s: No memory for sess_aif params on sess_id:%d, aif_id:%d\n", __func__, sess_obj->sess_id, aif_obj->aif_id);
	   ret = -EINVAL;
	   goto done;
   }

   memcpy(aif_obj->params, payload, size);
   aif_obj->params_size = size;

   if (sess_obj->state != SESSION_CLOSED && aif_obj->state >= AIF_OPENED) {
	   ret = graph_set_config(sess_obj->graph, aif_obj->params, aif_obj->params_size);
	   if (ret) {
		   AGM_LOGE("%s:Error:%d setting for sess_aif params on sess_id:%d, aif_id:%d\n",
				   __func__, ret, sess_obj->sess_id, aif_obj->aif_id);
		   free(aif_obj->params);
		   aif_obj->params = NULL;
		   aif_obj->params_size = 0;
	   }
   }

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;

}

int session_obj_set_sess_aif_params_with_tag(struct session_obj *sess_obj,
	uint32_t aif_id,
	struct agm_tag_config *tag_config)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	struct agm_meta_data_gsl *merged_metadata = NULL;
	struct agm_tag_config_gsl tag_config_gsl;

	pthread_mutex_lock(&sess_obj->lock);
	ret = aif_obj_get(sess_obj, aif_id, &aif_obj);
	if (ret) {
		AGM_LOGE("%s: Error obtaining aif object with sess_id:%d,  aif id:%d\n",
			__func__, sess_obj->sess_id, aif_id);
		goto done;
	}

	if (sess_obj->state == SESSION_CLOSED && aif_obj->state < AIF_OPENED) {
		AGM_LOGE("%s: Invalid state on sess_id:%d, aif_id:%d\n",
				 __func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -EINVAL;
		goto done;
	}

	merged_metadata = metadata_merge(3, &sess_obj->sess_meta, &aif_obj->sess_aif_meta, &aif_obj->dev_obj->metadata);
	if (!merged_metadata) {
		AGM_LOGE("%s: Error merging metadata session_id:%d aif_id:%d\n",
			__func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -ENOMEM;
		goto done;
	}

	tag_config_gsl.tag_id = tag_config->tag;
	tag_config_gsl.tkv.num_kvs = tag_config->num_tkvs;
	tag_config_gsl.tkv.kv = tag_config->kv;

	ret = graph_set_config_with_tag(sess_obj->graph, &merged_metadata->gkv, &tag_config_gsl);
	if (ret) {
		AGM_LOGE("%s:Error:%d setting for sess_aif params with tags on sess_id:%d, aif_id:%d\n",
				__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
	}

done:
	if (merged_metadata) {
		metadata_free(merged_metadata);
		free(merged_metadata);
	}

	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}

int session_obj_set_sess_aif_cal(struct session_obj *sess_obj,
	uint32_t aif_id,
	struct agm_cal_config *cal_config)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	struct agm_meta_data_gsl *merged_metadata = NULL;
	struct agm_key_vector_gsl ckv;

	pthread_mutex_lock(&sess_obj->lock);
	ret = aif_obj_get(sess_obj, aif_id, &aif_obj);
	if (ret) {
		AGM_LOGE("%s: Error obtaining aif object with sess_id:%d,  aif id:%d\n",
			__func__, sess_obj->sess_id, aif_id);
		goto done;
	}

	if (sess_obj->state == SESSION_CLOSED || aif_obj->state < AIF_OPENED) {
		AGM_LOGE("%s: Invalid state on sess_id:%d, aif_id:%d\n",
				 __func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -EINVAL;
		goto done;
	}

    ckv.kv = cal_config->kv;
    ckv.num_kvs = cal_config->num_ckvs;
    metadata_update_cal(&sess_obj->sess_meta, &ckv);
    metadata_update_cal(&aif_obj->sess_aif_meta, &ckv);
    metadata_update_cal(&aif_obj->dev_obj->metadata, &ckv);

	merged_metadata = metadata_merge(3, &sess_obj->sess_meta, &aif_obj->sess_aif_meta, &aif_obj->dev_obj->metadata);
	if (!merged_metadata) {
		AGM_LOGE("%s: Error merging metadata session_id:%d aif_id:%d\n",
			__func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -ENOMEM;
		goto done;
	}

	ret = graph_set_cal(sess_obj->graph, merged_metadata);
	if (ret) {
		AGM_LOGE("%s:Error:%d setting calibration on sess_id:%d, aif_id:%d\n",
				__func__, ret, sess_obj->sess_id, aif_obj->aif_id);
	}

done:
	if (merged_metadata) {
		metadata_free(merged_metadata);
		free(merged_metadata);
	}

	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}


int session_obj_set_sess_aif_metadata(struct session_obj *sess_obj,
	uint32_t aif_id,  uint32_t size, uint8_t *metadata)
{

	int ret = 0;
	struct aif *aif_obj = NULL;

	pthread_mutex_lock(&sess_obj->lock);
	ret = aif_obj_get(sess_obj, aif_id, &aif_obj);
	if (ret) {
		AGM_LOGE("%s: Error obtaining aif object with sess_id:%d,  aif id:%d\n",
			__func__, sess_obj->sess_id, aif_id);
		goto done;
	}

	ret = metadata_copy(&(aif_obj->sess_aif_meta), size, metadata);
	if (ret) {
		AGM_LOGE("%s: Error copying session audio interface metadata sess_id:%d, aif_id:%d \n",
			 __func__, sess_obj->sess_id, aif_obj->aif_id);
	}

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_get_sess_params(struct session_obj *sess_obj,
        void *payload, size_t size)
{
    int ret = 0;

    pthread_mutex_lock(&sess_obj->lock);

    if (sess_obj->state != SESSION_CLOSED) {
        ret = graph_get_config(sess_obj->graph, payload, size);
            if (ret)
                AGM_LOGE("%s:Error:%d get sess params on sess_id:%d\n",
                              __func__, ret, sess_obj->sess_id);
    }

done:
    pthread_mutex_unlock(&sess_obj->lock);
    return ret;
}

int session_obj_get_tag_with_module_info(struct session_obj *sess_obj, uint32_t aif_id, void *payload, size_t *size)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	struct agm_meta_data_gsl *merged_metadata = NULL;

	pthread_mutex_lock(&sess_obj->lock);
	ret = aif_obj_get(sess_obj, aif_id, &aif_obj);
	if (ret) {
		AGM_LOGE("%s: Error obtaining aif object with sess_id:%d,  aif id:%d\n",
			__func__, sess_obj->sess_id, aif_id);
		goto done;
	}

	merged_metadata = metadata_merge(3, &sess_obj->sess_meta, &aif_obj->sess_aif_meta, &aif_obj->dev_obj->metadata);
	if (!merged_metadata) {
		AGM_LOGE("%s: Error merging metadata session_id:%d aif_id:%d\n",
			__func__, sess_obj->sess_id, aif_obj->aif_id);
		ret = -ENOMEM;
		goto done;
	}

	ret = graph_get_tags_with_module_info(&merged_metadata->gkv, payload, size);
	if (ret) {
		AGM_LOGE("%s: Error getting tag with module info from graph for session_id:%d aif_id:%d\n",
			__func__, sess_obj->sess_id, aif_obj->aif_id);
		goto done;
	}

done:
	if (merged_metadata) {
		metadata_free(merged_metadata);
		free(merged_metadata);
	}

	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_register_cb(struct session_obj *sess_obj, agm_event_cb cb, enum event_type evt_type, void *client_data)
{
	int ret = 0;
	struct session_cb *sess_cb = NULL;


	pthread_mutex_lock(&sess_obj->cb_pool_lock);
	sess_cb = calloc(1, sizeof(struct session_cb));
	if (!sess_cb) {
		AGM_LOGV("%s: Error creating session_cb object with sess_id:%d\n",__func__, sess_obj->sess_id);
		ret = -ENOMEM;
		goto done;
	}
        if (cb != NULL) {
	    sess_cb->cb = cb;
	    sess_cb->client_data = client_data;
	    sess_cb->evt_type = evt_type;
		AGM_LOGV("sess_cb %p client_data %p evt_type %d", sess_cb, client_data, evt_type);
        list_add_tail(&sess_obj->cb_pool, &sess_cb->node);
        } else {
            struct session_cb *sess_cb;
            struct listnode *node, *next;
            list_for_each_safe(node, next, &sess_obj->cb_pool) {
                sess_cb = node_to_item(node, struct session_cb, node);
                if (sess_cb->evt_type == evt_type &&
                     sess_cb->client_data == client_data) {
                    AGM_LOGV("remove sess_cb %p client_data %p evt_type %d", sess_cb, sess_cb->client_data, sess_cb->evt_type);
                    list_remove(&sess_cb->node);
                    free(sess_cb);
                }
            }
       }
done:
	pthread_mutex_unlock(&sess_obj->cb_pool_lock);
	return ret;
}

int session_obj_register_for_events(struct session_obj *sess_obj, struct agm_event_reg_cfg *evt_reg_cfg)
{

	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);

	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s: Error registering for events, Session with sess_id:%d in invalid state:%d\n",
					 __func__, sess_obj->sess_id, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = graph_register_for_events(sess_obj->graph, evt_reg_cfg);
	if (ret) {
		AGM_LOGE("%s: Error:%d registering events with graph for sess_id:%d\n",
				__func__, ret, sess_obj->sess_id);
		goto done;
	}

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_sess_aif_connect(struct session_obj *sess_obj,
	uint32_t aif_id, bool aif_state)
{
	int ret = 0;
	struct aif *aif_obj = NULL;
	uint32_t opened_count = 0;

	pthread_mutex_lock(&sess_obj->lock);
	ret = aif_obj_get(sess_obj, aif_id, &aif_obj);
	if (ret) {
		AGM_LOGE("%s: Error obtaining aif object with sess_id:%d,  aif id:%d\n",
			 __func__, sess_obj->sess_id, aif_id);
		goto done;
	}

	if (((aif_state == true) && (aif_obj->state > AIF_OPENED)) ||
		((aif_state == false) && (aif_obj->state < AIF_OPEN))) {
		AGM_LOGE("%s AIF already in state %d\n", __func__, aif_obj->state);
		ret = -EALREADY;
		goto done;
	}

	opened_count = aif_obj_get_count_with_state(sess_obj, AIF_OPENED, false);

	if (aif_state == true) {
		//TODO: check if the assumption is correct
		//Assumption: Each of the following state assumes that there was an Audio Interface in Connect state.
		switch (sess_obj->state){
		case SESSION_OPENED:
			ret = session_connect_aif(sess_obj, aif_obj, opened_count);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to Connect device\n", __func__, ret);
				goto done;
			}
			aif_obj->state = AIF_OPENED;
			opened_count++;
			break;
		case SESSION_PREPARED:
		case SESSION_STOPPED:
			ret = session_connect_aif(sess_obj, aif_obj, opened_count);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to Connect device\n", __func__, ret);
				goto done;
			}
			aif_obj->state = AIF_OPENED;
			opened_count++;

			ret = session_prepare(sess_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to prepare device\n", __func__, ret);
				goto unwind;
			}

			break;
		case SESSION_STARTED:
			ret = session_connect_aif(sess_obj, aif_obj, opened_count);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to Connect device\n", __func__, ret);
				goto done;
			}
			aif_obj->state = AIF_OPENED;
			opened_count++;

			ret = session_prepare(sess_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to prepare device\n", __func__, ret);
				goto unwind;
			}
			ret = session_start(sess_obj);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to start device\n", __func__, ret);
				goto unwind;
			}
			break;

		case SESSION_CLOSED:
			aif_obj->state = AIF_OPEN;
			break;

		}

	} else {
		aif_obj->state = AIF_CLOSE;

	   //if session is in started state and more than 1 device is connect, call remove, if only 1 device is connected, do graph stop
		switch (sess_obj->state) {
		case SESSION_OPENED:
		case SESSION_PREPARED:
		case SESSION_STARTED:
		case SESSION_STOPPED:
			ret = session_disconnect_aif(sess_obj, aif_obj, opened_count);
			if (ret) {
				AGM_LOGE("%s Error:%d, Unable to Connect device\n", __func__, ret);
				goto done;
			}
		}
		aif_obj->state = AIF_CLOSED;
	}
	goto done;

unwind:
	session_disconnect_aif(sess_obj, aif_obj, opened_count);

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_open(uint32_t session_id, struct session_obj **session)
{

	struct session_obj *sess_obj = NULL;
	int ret = 0;

	ret = session_obj_get(session_id, &sess_obj);
	if (ret) {
		AGM_LOGE("%s: Error getting session object\n", __func__);
		return ret;
	}

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state != SESSION_CLOSED) {
		AGM_LOGE("%s: Session already Opened, session_state:%d\n", __func__, sess_obj->state);
		ret = -EALREADY;
		goto done;
	}

/*
	1. get first device obj from the list for the session
	2. concatenate stream+dev metadata
	3. for playback, open alsa first, open graph
	4. get rest of the devices, call add graph
	5. update state as opened
*/

	ret = session_open_with_first_device(sess_obj);
	if (ret) {
		AGM_LOGE("%s: Unable to open a session with Session ID:%d\n",  __func__, sess_obj->sess_id);
		goto done;
	}

	ret = session_connect_reminder_devices(sess_obj);
	if (ret) {
		AGM_LOGE("%s: Unable to open a session with Session ID:%d\n",  __func__, sess_obj->sess_id);
		goto done;
	}

	//configure loopback if loopback state is true
	if (sess_obj->loopback_state == true) {
		ret = session_set_loopback(sess_obj, sess_obj->loopback_sess_id, sess_obj->loopback_state);
		if (ret) {
			goto done;
		}
	}

	//configure ecref if valid session id has been set
	if (sess_obj->ec_ref_state == true) {
		ret = session_set_ec_ref(sess_obj, sess_obj->ec_ref_aif_id, sess_obj->ec_ref_state);
		if (ret) {
			goto done;
		}
	}

	sess_obj->state = SESSION_OPENED;
	*session = sess_obj;

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_set_config(struct session_obj *sess_obj, struct agm_session_config *stream_config,
	struct agm_media_config *media_config,
	struct agm_buffer_config *buffer_config)
{

	pthread_mutex_lock(&sess_obj->lock);

	sess_obj->stream_config = *stream_config;
	sess_obj->media_config = *media_config;
	sess_obj->buffer_config = *buffer_config;

	pthread_mutex_unlock(&sess_obj->lock);
	return 0;
}

int session_obj_prepare(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	ret = session_prepare(sess_obj);
	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}

int session_obj_start(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	ret = session_start(sess_obj);
	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}

int session_obj_stop(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	ret = session_stop(sess_obj);
	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}


int session_obj_close(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	ret = session_close(sess_obj);
	pthread_mutex_unlock(&sess_obj->lock);

	return ret;
}

int session_obj_pause(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	/* TODO: should pause be issued in specific state, for now ensure its in started state */
	if (sess_obj->state != SESSION_STARTED) {
		AGM_LOGE("%s Cannot issue pause in state:%d\n", __func__, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = graph_pause(sess_obj->graph);
	if (ret) {
		AGM_LOGE("%s Error:%d pausing graph\n", __func__, ret);
	}

	sess_obj->state = SESSION_PAUSED;

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_resume(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state != SESSION_PAUSED) {
		ret = -EINVAL;
		goto done;
	}

	ret = graph_resume(sess_obj->graph);
	if (ret) {
		AGM_LOGE("%s Error:%d resuming graph\n", __func__, ret);
	}

	sess_obj->state = SESSION_STARTED;

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_read(struct session_obj *sess_obj, void *buff, size_t *count)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s Cannot issue read in state:%d\n", __func__, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = graph_read(sess_obj->graph, buff, count);
	if (ret) {
		AGM_LOGE("%s Error:%d reading from graph\n", __func__, ret);
	}

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_write(struct session_obj *sess_obj, void *buff, size_t *count)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s Cannot issue write in state:%d\n", __func__, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = graph_write(sess_obj->graph, buff, count);
	if (ret) {
		AGM_LOGE("%s Error:%d writing to graph\n", __func__, ret);
	}

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

size_t session_obj_hw_processed_buff_cnt(struct session_obj *sess_obj, enum direction dir)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s Cannot issue resume in state:%d\n", __func__, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = (int)graph_get_hw_processed_buff_cnt(sess_obj->graph, dir);


done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_set_loopback(struct session_obj *sess_obj, uint32_t playback_sess_id, bool state)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (playback_sess_id == sess_obj->loopback_sess_id && state == sess_obj->loopback_state) {
		AGM_LOGE("%s: loopback already in %s state for session:%d\n",
				__func__, ((state != false) ? "enabled":"disabled"), sess_obj->sess_id);
		ret = -EALREADY;
		goto done;
	}

	/*
	 * loopback enables just the edge b/w capture and playback sessions.
	 * There is no need to call prepare/start on added graphs as their states are
	 * are updated as each of these session states are updated.
	 */

	switch(sess_obj->state) {
	case SESSION_OPENED:
	case SESSION_PREPARED:
	case SESSION_STARTED:
	case SESSION_STOPPED:
		if (state == true)
			ret = session_set_loopback(sess_obj, playback_sess_id, state);
		else
			ret = session_set_loopback(sess_obj, sess_obj->loopback_sess_id, state);

		if (ret) {
			AGM_LOGE("%s: Error:%d setting loopback state:%s for session:%d\n",
					__func__, ret, ((state != false) ? "enable":"disable"), sess_obj->sess_id);
			goto done;
		}

		break;

	case SESSION_CLOSED:
		break;
	}
	sess_obj->loopback_sess_id = playback_sess_id;
	sess_obj->loopback_state = state;

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_set_ec_ref(struct session_obj *sess_obj, uint32_t aif_id, bool state)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (aif_id == sess_obj->ec_ref_aif_id && state == sess_obj->ec_ref_state) {
		AGM_LOGE("%s: ec_ref already in %s state for session:%d\n",
				__func__, ((state != false) ? "enabled":"disabled"), sess_obj->sess_id);
		ret = -EALREADY;
		goto done;
	}

	/*
	 * ec_ref enables just the edge b/w capture and playback aif id.
	 * There is no need to call prepare/start on added graphs as their states are
	 * are updated as each of these session states are updated.
	 */

	switch(sess_obj->state) {
	case SESSION_OPENED:
	case SESSION_PREPARED:
	case SESSION_STARTED:
	case SESSION_STOPPED:
		if (state == true)
			ret = session_set_ec_ref(sess_obj, aif_id, state);
		else
			ret = session_set_ec_ref(sess_obj, sess_obj->ec_ref_aif_id, state);

		if (ret) {
			AGM_LOGE("%s: Error:%d setting ec_ref state:%s for session:%d\n",
					__func__, ret, ((state != false) ? "enable":"disable"), sess_obj->sess_id);
			goto done;
		}

		break;

	case SESSION_CLOSED:
		break;
	}
	sess_obj->ec_ref_aif_id = aif_id;
	sess_obj->ec_ref_state = state;

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}

int session_obj_eos(struct session_obj *sess_obj)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s Cannot issue EOS in state:%d\n", __func__, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = graph_eos(sess_obj->graph);
	if (ret) {
		AGM_LOGE("%s Error:%d sending EOS cmd \n", __func__, ret);
	}

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}


int session_obj_get_timestamp(struct session_obj *sess_obj, uint64_t *timestamp)
{
	int ret = 0;

	pthread_mutex_lock(&sess_obj->lock);
	if (sess_obj->state == SESSION_CLOSED) {
		AGM_LOGE("%s Cannot get timestamp in state:%d\n", __func__, sess_obj->state);
		ret = -EINVAL;
		goto done;
	}

	ret = graph_get_session_time(sess_obj->graph, timestamp);
	if (ret) {
		AGM_LOGE("%s Error:%d for get_timestamp \n", __func__, ret);
	}

done:
	pthread_mutex_unlock(&sess_obj->lock);
	return ret;
}
