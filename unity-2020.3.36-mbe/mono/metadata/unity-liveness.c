#include <config.h>
#include <glib.h>
#include <mono/metadata/object.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/domain-internals.h>
#include <mono/utils/mono-error.h>

typedef struct _LivenessState LivenessState;

typedef struct _GPtrArray custom_growable_array;
#define array_at_index(array,index) (array)->pdata[(index)]

#if defined(HAVE_SGEN_GC)
void sgen_stop_world (int generation);
void sgen_restart_world (int generation);
#elif defined(HAVE_BOEHM_GC)
#ifdef HAVE_BDWGC_GC
extern void GC_stop_world_external();
extern void GC_start_world_external();
#else
void GC_stop_world_external()
{
	g_assert_not_reached ();
}
void GC_start_world_external()
{
	g_assert_not_reached ();
}
#endif
#else
#error need to implement liveness GC API
#endif

custom_growable_array* array_create_and_initialize (guint capacity)
{
	custom_growable_array* array = g_ptr_array_sized_new(capacity);
	array->len = 0;
	return array;
}

gboolean array_is_full(custom_growable_array* array)
{
	return g_ptr_array_capacity(array) == array->len;
}

void array_destroy (custom_growable_array* array)
{
	g_ptr_array_free(array, TRUE);
}

void array_push_back(custom_growable_array* array, gpointer value)
{
	g_assert(!array_is_full(array));
	array->pdata[array->len] = value;
	array->len++;
}

gpointer array_pop_back(custom_growable_array* array)
{
	array->len--;
	return array->pdata[array->len];
}

void array_clear(custom_growable_array* array)
{
	array->len = 0;
}

void array_grow(custom_growable_array* array)
{
	int oldlen = array->len;
	g_ptr_array_set_size(array, g_ptr_array_capacity(array)*2);
	array->len = oldlen;
}

typedef void (*register_object_callback)(gpointer* arr, int size, void* callback_userdata);
typedef void (*WorldStateChanged)();
struct _LivenessState
{
	gint                first_index_in_all_objects;
	custom_growable_array* all_objects;

	MonoClass*          filter;

	custom_growable_array* process_array;
	guint               initial_alloc_count;

	void*               callback_userdata;

	register_object_callback filter_callback;
	WorldStateChanged        onWorldStartCallback;
	WorldStateChanged        onWorldStopCallback;
	guint               traverse_depth; // track recursion. Prevent stack overflow by limiting recurion
};

/* number of sub elements of an array to process before recursing
 * we take a depth first approach to use stack space rather than re-allocating
 * processing array which requires restarting world to ensure allocator lock is not held
*/
const int kArrayElementsPerChunk = 256;

/* how far we recurse processing array elements before we stop. Prevents stack overflow */
const int kMaxTraverseRecursionDepth = 128;

/* Liveness calculation */
MONO_API LivenessState* mono_unity_liveness_allocate_struct (MonoClass* filter, guint max_count, register_object_callback callback, void* callback_userdata, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback);
MONO_API void           mono_unity_liveness_stop_gc_world (LivenessState* state);
MONO_API void           mono_unity_liveness_finalize (LivenessState* state);
MONO_API void           mono_unity_liveness_start_gc_world (LivenessState* state);
MONO_API void           mono_unity_liveness_free_struct (LivenessState* state);

MONO_API LivenessState* mono_unity_liveness_calculation_begin (MonoClass* filter, guint max_count, register_object_callback callback, void* callback_userdata, WorldStateChanged onStartWorldCallback, WorldStateChanged onStopWorldCallback);
MONO_API void           mono_unity_liveness_calculation_end (LivenessState* state);

MONO_API void           mono_unity_liveness_calculation_from_root (MonoObject* root, LivenessState* state);
MONO_API void           mono_unity_liveness_calculation_from_statics (LivenessState* state);

#define MARK_OBJ(obj) \
	do { \
		(obj)->vtable = (MonoVTable*)(((gsize)(obj)->vtable) | (gsize)1); \
	} while (0)

#define CLEAR_OBJ(obj) \
	do { \
		(obj)->vtable = (MonoVTable*)(((gsize)(obj)->vtable) & ~(gsize)1); \
	} while (0)

#define IS_MARKED(obj) \
	(((gsize)(obj)->vtable) & (gsize)1)

#define GET_VTABLE(obj) \
	((MonoVTable*)(((gsize)(obj)->vtable) & ~(gsize)1))


void mono_filter_objects(LivenessState* state);

void mono_reset_state(LivenessState* state)
{
	state->first_index_in_all_objects = state->all_objects->len;
	array_clear(state->process_array);
}

void array_safe_grow(LivenessState* state, custom_growable_array* array)
{
	// if all_objects run out of space, run through list
	// clear bit in vtable, start the world, reallocate, stop the world and continue
	int i;
	for (i = 0; i < state->all_objects->len; i++)
	{
		MonoObject* object = array_at_index(state->all_objects,i);
		CLEAR_OBJ(object);
	}
	mono_unity_liveness_start_gc_world(state);
	array_grow(array);
	mono_unity_liveness_stop_gc_world (state);
	for (i = 0; i < state->all_objects->len; i++)
	{
		MonoObject* object = array_at_index(state->all_objects,i);
		MARK_OBJ(object);
	}
}

static gboolean should_process_value (MonoObject* val, MonoClass* filter)
{
	MonoClass* val_class = GET_VTABLE(val)->klass;
	if (filter && 
		!mono_class_has_parent (val_class, filter))
		return FALSE;

	return TRUE;
}

static void mono_traverse_array (MonoArray* array, LivenessState* state);
static void mono_traverse_object (MonoObject* object, LivenessState* state);
static void mono_validate_array (MonoArray* array, LivenessState* state);
static void mono_validate_object (MonoObject* object, LivenessState* state);
static void mono_traverse_gc_desc (MonoObject* object, LivenessState* state);
static void mono_traverse_objects (LivenessState* state);

static void mono_traverse_generic_object( MonoObject* object, LivenessState* state ) 
{
#ifdef HAVE_SGEN_GC
	gsize gc_desc = 0;
#else
	gsize gc_desc = (gsize)(GET_VTABLE(object)->gc_descr);
#endif

	if (gc_desc & (gsize)1)
		mono_traverse_gc_desc (object, state);
	else if (GET_VTABLE(object)->klass->rank)
		mono_traverse_array ((MonoArray*)object, state);
	else
		mono_traverse_object (object, state);
}

static void mono_traverse_and_validate_generic_object (MonoObject* object, LivenessState* state)
{
#ifdef HAVE_SGEN_GC
	gsize gc_desc = 0;
#else
	gsize gc_desc = (gsize)(GET_VTABLE (object)->gc_descr);
#endif

	if (GET_VTABLE (object)->klass->rank)
		mono_validate_array ((MonoArray*)object, state);
	else
		mono_validate_object (object, state);
}

static void validate_object_value (MonoObject* val, MonoType* storageType)
{
	if (val && storageType->type == MONO_TYPE_CLASS) {
		MonoClass* storageClass = storageType->data.klass;
		MonoClass* valClass = GET_VTABLE (val)->klass;
		if (mono_class_is_interface (storageClass)) {
			int found = 0;
			for (int i = 0; i < valClass->interface_offsets_count; ++i)
			{
				if (valClass->interfaces_packed[i] == storageClass)
				{
					found = TRUE;
					break;
				}
			}
			g_assert (found);
		}
		else {
			int res = mono_class_has_parent_fast (valClass, storageClass);
			g_assert (res);
		}
	}
}


static gboolean mono_add_process_object (MonoObject* object, LivenessState* state)
{
	if (object && !IS_MARKED(object))
	{
		gboolean has_references = GET_VTABLE(object)->klass->has_references;
		if(has_references || should_process_value(object,state->filter))
		{
			if (array_is_full(state->all_objects))
				array_safe_grow(state, state->all_objects);
			array_push_back(state->all_objects, object);
			MARK_OBJ(object);
		}
		// Check if klass has further references - if not skip adding
		if (has_references)
		{
			if(array_is_full(state->process_array))
				array_safe_grow(state, state->process_array);
			array_push_back(state->process_array, object);
			return TRUE;
		}
	}

	return FALSE;
}

MONO_API void mono_validate_object_pointer (MonoObject* object)
{
	if (object)
	{
		MonoVTable* vtable = NULL;
		MonoClass* klass = NULL;
		char* name = NULL;

		vtable = object->vtable;
		klass = vtable->klass;
		name = klass->name;

		g_assert(name);
	}
}

MONO_API void mono_validate_string_pointer(MonoString* string)
{
	mono_validate_object_pointer(&string->object);
}

static gboolean mono_add_and_validate_object (MonoObject* object, LivenessState* state)
{
	if (object)
	{
		MonoVTable* vtable = NULL;
		MonoClass* klass = NULL;
		char *name = NULL;

		vtable = GET_VTABLE(object);
		klass = vtable->klass;
		name = klass->name;

		g_assert(name);

		if (!IS_MARKED (object))
		{
			gboolean has_references = GET_VTABLE (object)->klass->has_references;
			if (has_references || should_process_value (object, state->filter))
			{
				if (array_is_full (state->all_objects))
					array_safe_grow (state, state->all_objects);
				array_push_back (state->all_objects, object);
				MARK_OBJ (object);
			}
			// Check if klass has further references - if not skip adding
			if (has_references)
			{
				if (array_is_full (state->process_array))
					array_safe_grow (state, state->process_array);
				array_push_back (state->process_array, object);
				return TRUE;
			}
		}
	}

	return FALSE;
}

static gboolean mono_field_can_contain_references(MonoClassField* field)
{
	if (MONO_TYPE_ISSTRUCT(field->type))
		return TRUE;
	if (field->type->attrs & FIELD_ATTRIBUTE_LITERAL)
		return FALSE;
	if (field->type->type == MONO_TYPE_STRING)
		return FALSE;
	return MONO_TYPE_IS_REFERENCE(field->type);
}

static gboolean mono_traverse_object_internal (MonoObject* object, gboolean isStruct, MonoClass* klass, LivenessState* state)
{
	int i;
	MonoClassField *field;
	MonoClass *p;
	gboolean added_objects = FALSE;

	g_assert (object);
	
	// subtract the added offset for the vtable. This is added to the offset even though it is a struct
	if(isStruct)
		object--;

	for (p = klass; p != NULL; p = p->parent)
	{
		if (p->size_inited == 0)
			continue;
		for (i = 0; i < mono_class_get_field_count (p); i++)
		{
			field = &p->fields[i];
			if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			if(!mono_field_can_contain_references(field))
				continue;

			if (MONO_TYPE_ISSTRUCT(field->type))
			{
				char* offseted = (char*)object;
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST)
				{
					g_assert(field->type->data.generic_class->cached_class);
					added_objects |= mono_traverse_object_internal((MonoObject*)offseted, TRUE, field->type->data.generic_class->cached_class, state);
				}
				else
					added_objects |= mono_traverse_object_internal((MonoObject*)offseted, TRUE, field->type->data.klass, state);
				continue;
			}

			if (field->offset == -1) {
				g_assert_not_reached ();
			} else {
				MonoObject* val = NULL;
				MonoVTable *vtable = NULL;
				mono_field_get_value (object, field, &val);
				added_objects |= mono_add_process_object (val, state);
			}
		}
	}

	return added_objects;
}

static gboolean mono_validate_object_internal (MonoObject* object, gboolean isStruct, MonoClass* klass, LivenessState* state)
{
	int i;
	MonoClassField* field;
	MonoClass* p;
	gboolean added_objects = FALSE;

	g_assert (object);

	// subtract the added offset for the vtable. This is added to the offset even though it is a struct
	if (isStruct)
		object--;

	for (p = klass; p != NULL; p = p->parent)
	{
		if (p->size_inited == 0)
			continue;
		for (i = 0; i < mono_class_get_field_count (p); i++)
		{
			field = &p->fields[i];
			if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			if (!mono_field_can_contain_references (field))
				continue;

			if (MONO_TYPE_ISSTRUCT (field->type))
			{
				char* offseted = (char*)object;
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST)
				{
					g_assert (field->type->data.generic_class->cached_class);
					added_objects |= mono_validate_object_internal ((MonoObject*)offseted, TRUE, field->type->data.generic_class->cached_class, state);
				}
				else
					added_objects |= mono_validate_object_internal ((MonoObject*)offseted, TRUE, field->type->data.klass, state);
				continue;
			}

			if (field->offset == -1) {
				g_assert_not_reached ();
			}
			else {
				MonoObject *val = NULL;
				MonoVTable *vtable = NULL;
				mono_field_get_value (object, field, &val);
				added_objects |= mono_add_and_validate_object (val, state);
				validate_object_value (val, field->type);
			}
		}
	}

	return added_objects;
}

static void mono_traverse_object (MonoObject* object, LivenessState* state)
{
	mono_traverse_object_internal (object, FALSE, GET_VTABLE(object)->klass, state);
}

static void mono_validate_object (MonoObject* object, LivenessState* state)
{
	mono_validate_object_internal (object, FALSE, GET_VTABLE (object)->klass, state);
}

static void mono_traverse_gc_desc (MonoObject* object, LivenessState* state)
{
#define WORDSIZE ((int)sizeof(gsize)*8)
	int i = 0;
	gsize mask = (gsize)(GET_VTABLE(object)->gc_descr);

	g_assert (mask & (gsize)1);

	for (i = 0; i < WORDSIZE-2; i++)
	{
		gsize offset = ((gsize)1 << (WORDSIZE - 1 - i));
		if (mask & offset)
		{
			MonoObject* val = *(MonoObject**)(((char*)object) + i * sizeof(void*));
			mono_add_process_object(val, state);
		}
	}
}

static void mono_traverse_objects (LivenessState* state)
{
	int i = 0;
	MonoObject* object = NULL;

	state->traverse_depth++;
	while (state->process_array->len > 0)
	{
		object = array_pop_back(state->process_array);
		mono_traverse_generic_object(object, state);
	}
	state->traverse_depth--;
}

static void mono_traverse_and_validate_objects (LivenessState* state)
{
	int i = 0;
	MonoObject* object = NULL;

	state->traverse_depth++;
	while (state->process_array->len > 0)
	{
		object = array_pop_back (state->process_array);
		mono_traverse_and_validate_generic_object (object, state);
	}
	state->traverse_depth--;
}

static gboolean should_traverse_objects (size_t index, gint32 recursion_depth)
{
	// Add kArrayElementsPerChunk objects at a time and then traverse
	return ((index + 1) & (kArrayElementsPerChunk - 1)) == 0 && 
		recursion_depth < kMaxTraverseRecursionDepth;
}

static void mono_traverse_array (MonoArray* array, LivenessState* state)
{
	size_t i = 0;
	gboolean has_references;
	MonoObject* object = (MonoObject*)array;
	MonoClass* element_class;
	size_t elementClassSize;
	size_t array_length;
	
	g_assert (object);
	
	
	
	element_class = GET_VTABLE(object)->klass->element_class;
	has_references = !mono_class_is_valuetype(element_class);
	g_assert(element_class->size_inited != 0);
	
	for (i = 0; i < mono_class_get_field_count (element_class); i++)
	{
		has_references |= mono_field_can_contain_references(&element_class->fields[i]);
	}
	
	if (!has_references)
		return;
	
	array_length = mono_array_length (array);
	if (element_class->valuetype)
	{
		size_t items_processed = 0;
		elementClassSize = mono_class_array_element_size (element_class);
		for (i = 0; i < array_length; i++)
		{
			MonoObject* object = (MonoObject*)mono_array_addr_with_size (array, elementClassSize, i);
			if (mono_traverse_object_internal (object, 1, element_class, state))
				items_processed++;
			
			if(should_traverse_objects (items_processed, state->traverse_depth))
				mono_traverse_objects(state);
		}
	}
	else
	{
		size_t items_processed = 0;
		for (i = 0; i < array_length; i++)
		{
			MonoObject* val =  mono_array_get(array, MonoObject*, i);
			if (mono_add_process_object (val, state))
				items_processed++;
			
			if (should_traverse_objects (items_processed, state->traverse_depth))
				mono_traverse_objects(state);
		}
	}
}


static void mono_validate_array (MonoArray* array, LivenessState* state)
{
	size_t i = 0;
	gboolean has_references;
	MonoObject* object = (MonoObject*)array;
	MonoClass* element_class;
	size_t elementClassSize;
	size_t array_length;

	g_assert (object);



	element_class = GET_VTABLE (object)->klass->element_class;
	has_references = !mono_class_is_valuetype (element_class);
	g_assert (element_class->size_inited != 0);

	for (i = 0; i < mono_class_get_field_count (element_class) && !has_references; i++)
	{
		has_references |= mono_field_can_contain_references (&element_class->fields[i]);
	}

	if (!has_references)
		return;

	array_length = mono_array_length (array);
	if (element_class->valuetype)
	{
		size_t items_processed = 0;
		elementClassSize = mono_class_array_element_size (element_class);
		for (i = 0; i < array_length; i++)
		{
			MonoObject* object = (MonoObject*)mono_array_addr_with_size (array, elementClassSize, i);
			if (mono_validate_object_internal (object, 1, element_class, state))
				items_processed++;

			if (should_traverse_objects (items_processed, state->traverse_depth))
				mono_traverse_and_validate_objects (state);
		}
	}
	else
	{
		size_t items_processed = 0;
		for (i = 0; i < array_length; i++)
		{
			MonoObject* val = mono_array_get (array, MonoObject*, i);
			if (mono_add_and_validate_object (val, state))
				items_processed++;

			validate_object_value (val, &element_class->byval_arg);

			if (should_traverse_objects (items_processed, state->traverse_depth))
				mono_traverse_and_validate_objects (state);
		}
	}
}

void mono_filter_objects(LivenessState* state)
{
	gpointer filtered_objects[64];
	gint num_objects = 0;

	int i = state->first_index_in_all_objects;
	for ( ; i < state->all_objects->len; i++)
	{
		MonoObject* object = state->all_objects->pdata[i];
		if (should_process_value (object, state->filter))
			filtered_objects[num_objects++] = object;
		if (num_objects == 64)
		{
			state->filter_callback(filtered_objects, 64, state->callback_userdata);
			num_objects = 0;
		}
	}

	if (num_objects != 0)
		state->filter_callback(filtered_objects, num_objects, state->callback_userdata);
}

/**
 * mono_unity_liveness_calculation_from_statics:
 *
 * Returns an array of MonoObject* that are reachable from the static roots
 * in the current domain and derive from @filter (if not NULL).
 */
void mono_unity_liveness_calculation_from_statics(LivenessState* liveness_state)
{
	int i, j;
	MonoDomain* domain = mono_domain_get();

	mono_reset_state(liveness_state);


	for (i = 0; i < domain->class_vtable_array->len; ++i)
	{
		MonoVTable* vtable = (MonoVTable *)g_ptr_array_index (domain->class_vtable_array, i);
		MonoClass* klass = vtable->klass;
		MonoClassField *field;
		if (!klass)
			continue;
		if (!klass->has_static_refs)
			continue;
		if (klass->image == mono_defaults.corlib)
			continue;
		if (klass->size_inited == 0)
			continue;
		for (j = 0; j < mono_class_get_field_count (klass); j++)
		{
			field = &klass->fields[j];
			if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC))
				continue;
			if(!mono_field_can_contain_references(field))
				continue;
			// shortcut check for special statics
			if (field->offset == -1)
				continue;

			if (MONO_TYPE_ISSTRUCT(field->type))
			{
				char* offseted = (char*)mono_vtable_get_static_field_data (vtable);
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST)
				{
					g_assert(field->type->data.generic_class->cached_class);
					mono_traverse_object_internal((MonoObject*)offseted, TRUE, field->type->data.generic_class->cached_class, liveness_state);
				}
				else
				{
					mono_traverse_object_internal((MonoObject*)offseted, TRUE, field->type->data.klass, liveness_state);
				}
			}
			else
			{
				MonoError error;
				MonoObject* val = NULL;

				mono_field_static_get_value_checked (mono_class_vtable (domain, klass), field, &val, &error);

				if (val && mono_error_ok (&error))
				{
					mono_add_process_object(val, liveness_state);
				}
				mono_error_cleanup (&error);
			}
		}
	}
	mono_traverse_objects (liveness_state);
	//Filter objects and call callback to register found objects
	mono_filter_objects(liveness_state);
}

void mono_unity_liveness_add_object_callback(gpointer* objs, gint count, void* arr)
{
	int i;
	GPtrArray* objects = (GPtrArray*)arr;
	for (i = 0; i < count; i++)
	{
		if (g_ptr_array_capacity(objects) > objects->len)
			objects->pdata[objects->len++] = objs[i];
	}
}

/**
 * mono_unity_liveness_calculation_from_statics_managed:
 *
 * Returns a gchandle to an array of MonoObject* that are reachable from the static roots
 * in the current domain and derive from type retrieved from @filter_handle (if not NULL).
 */
gpointer mono_unity_liveness_calculation_from_statics_managed(gpointer filter_handle, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback)
{
	int i = 0;
	MonoArray *res = NULL;
	MonoReflectionType* filter_type = (MonoReflectionType*)mono_gchandle_get_target (GPOINTER_TO_UINT(filter_handle));
	MonoClass* filter = NULL;
	GPtrArray* objects = NULL;
	LivenessState* liveness_state = NULL;
	MonoError* error = NULL;

	if (filter_type)
		filter = mono_class_from_mono_type (filter_type->type);

	objects = g_ptr_array_sized_new(1000);
	objects->len = 0;

	liveness_state = mono_unity_liveness_calculation_begin (filter, 1000, mono_unity_liveness_add_object_callback, (void*)objects, onWorldStartCallback, onWorldStopCallback);

	mono_unity_liveness_calculation_from_statics (liveness_state);

	mono_unity_liveness_calculation_end (liveness_state);

	res = mono_array_new_checked (mono_domain_get (), filter ? filter: mono_defaults.object_class, objects->len, error);
	for (i = 0; i < objects->len; ++i) {
		MonoObject* o = g_ptr_array_index (objects, i);
		mono_array_setref (res, i, o);
	}
	g_ptr_array_free (objects, TRUE);

	
	return (gpointer)mono_gchandle_new ((MonoObject*)res, FALSE);

}

void mono_unity_heap_validation_from_statics (LivenessState* liveness_state);
#if HAVE_BOEHM_GC
extern void mono_gc_strong_handle_foreach (GFunc func, gpointer user_data);
extern void mono_gc_handle_lock ();
extern void mono_gc_handle_unlock ();
#endif

MONO_API void mono_unity_heap_validation ()
{

	int i = 0;
	MonoArray* res = NULL;
	GPtrArray* objects = NULL;
	LivenessState* liveness_state = NULL;
	MonoError* error = NULL;

	objects = g_ptr_array_sized_new (100000);
	objects->len = 0;

#if HAVE_BOEHM_GC
	mono_gc_handle_lock ();
#else
	g_assert_not_reached();
#endif

	liveness_state = mono_unity_liveness_calculation_begin (NULL, 100000, mono_unity_liveness_add_object_callback, (void*)objects, NULL, NULL);

	mono_unity_heap_validation_from_statics (liveness_state);

	mono_unity_liveness_calculation_end (liveness_state);

#if HAVE_BOEHM_GC
	mono_gc_handle_unlock ();
#endif // would get caught above by the assert

	g_ptr_array_free (objects, TRUE);
}

void gchandle_process (void* data, void* user_data)
{
	MonoObject* target = data;
	LivenessState* liveness_state = user_data;

	mono_add_and_validate_object (target, liveness_state);
}


static void
foreach_thread_static_field (gpointer key, gpointer value, gpointer user_data)
{
	MonoClassField* field = key;
	guint32 offset = GPOINTER_TO_UINT (value);
	LivenessState* liveness_state = user_data;

	if (!mono_field_can_contain_references (field))
		return;
	// TODO
	if (MONO_TYPE_ISSTRUCT (field->type))
		return;

	MonoInternalThread* thread;

	thread = mono_thread_internal_current ();

	gpointer data = mono_get_special_static_data_for_thread (thread, offset);

	MonoObject* val = *(MonoObject**)data;

	if (val) {
		mono_add_and_validate_object (val, liveness_state);
		validate_object_value (val, field->type);
	}
}

void mono_unity_heap_validation_from_statics (LivenessState* liveness_state)
{
	int i, j;
	MonoDomain* domain = mono_domain_get ();

	if (!domain)
		return;

	mono_reset_state (liveness_state);

#if HAVE_BOEHM_GC
	mono_gc_strong_handle_foreach (gchandle_process, liveness_state);
#else
	g_assert_not_reached();
#endif

	g_hash_table_foreach (domain->special_static_fields, foreach_thread_static_field, liveness_state);

	for (i = 0; i < domain->class_vtable_array->len; ++i)
	{
		MonoVTable* vtable = (MonoVTable*)g_ptr_array_index (domain->class_vtable_array, i);
		MonoClass* klass = vtable->klass;
		MonoClassField* field;
		if (!klass)
			continue;
		if (!klass->has_static_refs)
			continue;
		if (klass->image == mono_defaults.corlib)
			continue;
		if (klass->size_inited == 0)
			continue;
		for (j = 0; j < mono_class_get_field_count (klass); j++)
		{
			field = &klass->fields[j];
			if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC))
				continue;
			if (!mono_field_can_contain_references (field))
				continue;
			// shortcut check for special statics
			if (field->offset == -1)
				continue;

			if (MONO_TYPE_ISSTRUCT (field->type))
			{
				char* offseted = (char*)mono_vtable_get_static_field_data (vtable);
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST)
				{
					g_assert (field->type->data.generic_class->cached_class);
					mono_validate_object_internal ((MonoObject*)offseted, TRUE, field->type->data.generic_class->cached_class, liveness_state);
				}
				else
				{
					mono_validate_object_internal ((MonoObject*)offseted, TRUE, field->type->data.klass, liveness_state);
				}
			}
			else
			{
				MonoError error;
				MonoObject* val = NULL;

				mono_field_static_get_value_checked (mono_class_vtable (domain, klass), field, &val, &error);

				if (val && mono_error_ok (&error))
				{
					mono_add_and_validate_object (val, liveness_state);
				}
				mono_error_cleanup (&error);
			}
		}
	}
	mono_traverse_and_validate_objects (liveness_state);
}

/**
 * mono_unity_liveness_calculation_from_root:
 *
 * Returns an array of MonoObject* that are reachable from @root
 * in the current domain and derive from @filter (if not NULL).
 */
void mono_unity_liveness_calculation_from_root (MonoObject* root, LivenessState* liveness_state)
{
	mono_reset_state (liveness_state);

	array_push_back (liveness_state->process_array,root);

	mono_traverse_objects (liveness_state);

	//Filter objects and call callback to register found objects
	mono_filter_objects (liveness_state);
}

/**
 * mono_unity_liveness_calculation_from_root_managed:
 *
 * Returns a gchandle to an array of MonoObject* that are reachable from the static roots
 * in the current domain and derive from type retrieved from @filter_handle (if not NULL).
 */
gpointer mono_unity_liveness_calculation_from_root_managed(gpointer root_handle, gpointer filter_handle, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback)
{
	int i = 0;
	MonoArray *res = NULL;
	MonoReflectionType* filter_type = (MonoReflectionType*)mono_gchandle_get_target (GPOINTER_TO_UINT(filter_handle));
	MonoObject* root = mono_gchandle_get_target (GPOINTER_TO_UINT(root_handle));
	MonoClass* filter = NULL;
	GPtrArray* objects = NULL;
	LivenessState* liveness_state = NULL;
	MonoError* error = NULL;

	objects = g_ptr_array_sized_new(1000);
	objects->len = 0;

	if (filter_type)
		filter = mono_class_from_mono_type (filter_type->type);

	liveness_state = mono_unity_liveness_calculation_begin (filter, 1000, mono_unity_liveness_add_object_callback, (void*)objects, onWorldStartCallback, onWorldStopCallback);

	mono_unity_liveness_calculation_from_root (root, liveness_state);

	mono_unity_liveness_calculation_end (liveness_state);

	res = mono_array_new_checked (mono_domain_get (), filter ? filter: mono_defaults.object_class, objects->len, error);
	for (i = 0; i < objects->len; ++i) {
		MonoObject* o = g_ptr_array_index (objects, i);
		mono_array_setref (res, i, o);
	}

	g_ptr_array_free (objects, TRUE);

	return (gpointer)mono_gchandle_new ((MonoObject*)res, FALSE);
}

LivenessState* mono_unity_liveness_allocate_struct (MonoClass* filter, guint max_count, register_object_callback callback, void* callback_userdata, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback)
{
	LivenessState* state = NULL;

	// construct liveness_state;
	// allocate memory for the following structs
	// all_objects: contains a list of all referenced objects to be able to clean the vtable bits after the traversal
	// process_array. array that contains the objcets that should be processed. this should run depth first to reduce memory usage
	// if all_objects run out of space, run through list, add objects that match the filter, clear bit in vtable and then clear the array.

	state = g_new0(LivenessState, 1);
	max_count = max_count < 1000 ? 1000 : max_count;
	state->all_objects = array_create_and_initialize(max_count*4);
	state->process_array = array_create_and_initialize (max_count);

	state->first_index_in_all_objects = 0; 
	state->filter = filter;
	state->traverse_depth = 0;

	state->callback_userdata = callback_userdata;
	state->filter_callback = callback;
	state->onWorldStartCallback = onWorldStartCallback;
	state->onWorldStopCallback = onWorldStopCallback;

	return state;
}

void mono_unity_liveness_finalize (LivenessState* state)
{
	int i;
	for (i = 0; i < state->all_objects->len; i++)
	{
		MonoObject* object = g_ptr_array_index(state->all_objects,i);
		CLEAR_OBJ(object);
	}
}

void mono_unity_liveness_free_struct (LivenessState* state)
{
	//cleanup the liveness_state
	array_destroy(state->all_objects);
	array_destroy(state->process_array);
	g_free(state);
}

void mono_unity_liveness_stop_gc_world (LivenessState* state)
{
	if (state->onWorldStopCallback)
		state->onWorldStopCallback();
#if defined(HAVE_SGEN_GC)
	sgen_stop_world (1);
#elif defined(HAVE_BOEHM_GC)
	GC_stop_world_external ();
#else
#error need to implement liveness GC API
#endif
}

void mono_unity_liveness_start_gc_world (LivenessState* state)
{
#if defined(HAVE_SGEN_GC)
	sgen_restart_world (1);
#elif defined(HAVE_BOEHM_GC)
	GC_start_world_external ();
#else
#error need to implement liveness GC API
#endif
	if (state->onWorldStartCallback)
		state->onWorldStartCallback();
}

LivenessState* mono_unity_liveness_calculation_begin (MonoClass* filter, guint max_count, register_object_callback callback, void* callback_userdata, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback)
{
	LivenessState* state = mono_unity_liveness_allocate_struct (filter, max_count, callback, callback_userdata, onWorldStartCallback, onWorldStopCallback);
	mono_unity_liveness_stop_gc_world (state);
	// no allocations can happen beyond this point
	return state;
}

void mono_unity_liveness_calculation_end (LivenessState* state)
{
	mono_unity_liveness_finalize(state);
	mono_unity_liveness_start_gc_world(state);
	mono_unity_liveness_free_struct(state);
}
