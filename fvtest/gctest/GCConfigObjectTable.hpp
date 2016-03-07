/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2015, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#include "gcTestHelpers.hpp"

typedef struct ObjectEntry {
	const char *name;
	omrobjectptr_t objPtr;
	int32_t numOfRef;
} ObjectEntry;

class GCConfigObjectTable {
private:
	J9HashTable *objectTable;
	OMRPortLibrary *portLib;

public:
	GCConfigObjectTable(OMRPortLibrary *portLibrary) : objectTable(NULL), portLib(portLibrary) {}
	void create();
	void free();
	int32_t find(ObjectEntry **entry, const char *name);
	int32_t add(const char *name, omrobjectptr_t obj, int32_t numOfRef);
};