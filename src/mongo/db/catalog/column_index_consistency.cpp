/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/catalog/column_index_consistency.h"
#include "mongo/db/concurrency/exception_util.h"

namespace mongo {

int64_t ColumnIndexConsistency::traverseIndex(OperationContext* opCtx,
                                              const IndexCatalogEntry* index,
                                              ProgressMeterHolder& _progress,
                                              ValidateResults* results) {
    const auto indexName = index->descriptor()->indexName();

    // Ensure that this index has an open index cursor.
    const auto csiCursorIt = _validateState->getColumnStoreCursors().find(indexName);
    invariant(csiCursorIt != _validateState->getColumnStoreCursors().end());

    const std::unique_ptr<ColumnStore::Cursor>& csiCursor = csiCursorIt->second;

    int64_t numIndexEntries = 0;
    // Traverse the index in this for loop.
    for (auto cell = csiCursor->seekAtOrPast("", ColumnStore::kNullRowId); cell;
         cell = csiCursor->next()) {
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->hit();
        }
        ++numIndexEntries;

        if (numIndexEntries % kInterruptIntervalNumRecords == 0) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yield(opCtx);
        }

        if (_firstPhase) {
            addIndexEntry(cell.get());
        } else {
            _updateSuspectList(cell.get(), results);
        }
    }

    return numIndexEntries;
}

void ColumnIndexConsistency::traverseRecord(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            const IndexCatalogEntry* index,
                                            const RecordId& recordId,
                                            const BSONObj& record,
                                            ValidateResults* results) {
    ColumnStoreAccessMethod* csam = dynamic_cast<ColumnStoreAccessMethod*>(index->accessMethod());

    // Shred the record.
    csam->getKeyGen().visitCellsForInsert(
        record, [&](PathView path, const column_keygen::UnencodedCellView& unencodedCell) {
            _cellBuilder.reset();
            column_keygen::writeEncodedCell(unencodedCell, &_cellBuilder);
            tassert(
                7106112, "RecordID cannot be a string for column store indexes", !recordId.isStr());
            const auto cell = FullCellView{
                path, recordId.getLong(), CellView(_cellBuilder.buf(), _cellBuilder.len())};
            if (_firstPhase) {
                addDocEntry(cell);
            } else {
                _updateSuspectList(cell, results);
            }
        });
}

void ColumnIndexConsistency::_addIndexEntryErrors(OperationContext* opCtx,
                                                  const IndexCatalogEntry* index,
                                                  ValidateResults* results) {

    if (_firstPhase) {
        return;
    }

    const auto indexName = index->descriptor()->indexName();

    // Ensure that this index has an open index cursor.
    const auto csiCursorIt = _validateState->getColumnStoreCursors().find(indexName);
    invariant(csiCursorIt != _validateState->getColumnStoreCursors().end());

    const auto& csiCursor = csiCursorIt->second;

    const ColumnStoreAccessMethod* csam =
        dynamic_cast<ColumnStoreAccessMethod*>(index->accessMethod());

    for (const auto rowId : _suspects) {
        // Gather all paths for this RecordId.
        StringMap<int> paths;
        PathValue nextPath = "";
        while (auto next = csiCursor->seekAtOrPast(nextPath, rowId)) {
            if (next->rid < rowId) {
                nextPath.assign(next->path.rawData(), next->path.size());
                continue;
            }

            if (next->rid == rowId) {
                paths[next->path]++;
            }
            nextPath.assign(next->path.rawData(), next->path.size());
            nextPath += '\x01';  // Next possible path (\0 is not allowed).
        }

        const RecordId recordId(rowId);
        const auto& rowStoreCursor = _validateState->getTraverseRecordStoreCursor();
        if (const auto record = rowStoreCursor->seekExact(opCtx, recordId); record) {
            // Shred the document and check each path/value against csi(path, rowId).
            csam->getKeyGen().visitCellsForInsert(
                record->data.toBson(),
                [&](PathView path, const column_keygen::UnencodedCellView& cell) {
                    _cellBuilder.reset();
                    column_keygen::writeEncodedCell(cell, &_cellBuilder);
                    const auto rowCell =
                        FullCellView{path, rowId, CellView(_cellBuilder.buf(), _cellBuilder.len())};

                    const auto csiCell = csiCursor->seekExact(path, rowId);
                    if (!csiCell) {
                        // Rowstore has entry that index doesn't
                        _missingIndexEntries.emplace_back(rowCell);
                        results->missingIndexEntries.push_back(
                            _generateInfo(csam->indexName(), recordId, path, rowId));
                    } else if (csiCell->value != rowCell.value) {
                        // Rowstore and index values diverge
                        _extraIndexEntries.emplace_back(csiCell.get());
                        _missingIndexEntries.emplace_back(rowCell);
                        results->missingIndexEntries.push_back(_generateInfo(
                            csam->indexName(), recordId, path, rowId, csiCell->value));
                        results->extraIndexEntries.push_back(
                            _generateInfo(csam->indexName(), recordId, path, rowId, rowCell.value));
                    }
                    if (paths.count(rowCell.path) == 1) {
                        paths.erase(rowCell.path);
                    } else {
                        paths[rowCell.path]--;
                    }
                });
        }

        // Extra paths in index that don't exist in the row-store.
        for (const auto& kv : paths) {
            for (int i = 0; i < kv.second; i++) {
                _extraIndexEntries.emplace_back(kv.first, rowId, ""_sd);
                results->extraIndexEntries.push_back(
                    _generateInfo(csam->indexName(), recordId, kv.first, rowId));
            }
        }
    }

    if (!_missingIndexEntries.empty() || !_extraIndexEntries.empty()) {
        StringBuilder ss;
        ss << "Index with name '" << csam->indexName() << "' has inconsistencies.";
        results->errors.push_back(ss.str());
        results->indexResultsMap.at(csam->indexName()).valid = false;
    }
    if (!_missingIndexEntries.empty()) {
        StringBuilder ss;
        ss << "Detected " << _missingIndexEntries.size() << " missing index entries.";
        results->warnings.push_back(ss.str());
        results->valid = false;
    }
    if (!_extraIndexEntries.empty()) {
        StringBuilder ss;
        ss << "Detected " << _extraIndexEntries.size() << " extra index entries.";
        results->warnings.push_back(ss.str());
        results->valid = false;
    }
}

void ColumnIndexConsistency::addIndexEntryErrors(OperationContext* opCtx,
                                                 ValidateResults* results) {
    int numColumnStoreIndexes = 0;
    for (const auto& index : _validateState->getIndexes()) {
        const IndexDescriptor* descriptor = index->descriptor();
        if (descriptor->getAccessMethodName() == IndexNames::COLUMN) {
            ++numColumnStoreIndexes;
            uassert(7106138,
                    "The current implementation only supports a single column-store index.",
                    numColumnStoreIndexes <= 1);
            _addIndexEntryErrors(opCtx, index.get(), results);
        }
    }
}

void ColumnIndexConsistency::repairIndexEntries(OperationContext* opCtx, ValidateResults* results) {
    // TODO SERVER-71560 Repair should be implemented.
}

void ColumnIndexConsistency::validateIndexKeyCount(OperationContext* opCtx,
                                                   const IndexCatalogEntry* index,
                                                   long long* numRecords,
                                                   IndexValidateResults& results) {
    // Nothing to do.
}

// Generate info for the validate() command output.
BSONObj ColumnIndexConsistency::_generateInfo(const std::string& indexName,
                                              const RecordId& recordId,
                                              const PathView path,
                                              RowId rowId,
                                              StringData value) {
    BSONObjBuilder infoBuilder;

    infoBuilder.append("indexName", indexName);
    recordId.serializeToken("recordId", &infoBuilder);
    if (rowId != ColumnStore::kNullRowId) {
        infoBuilder.append("rowId", rowId);
    }
    if (!path.empty()) {
        infoBuilder.append("indexPath", path);
    }
    if (!value.empty()) {
        infoBuilder.append("value", value);
    }
    return infoBuilder.obj();
}

void ColumnIndexConsistency::addDocEntry(const FullCellView& val) {
    _tabulateEntry(val, 1);
    _numDocs++;
}

void ColumnIndexConsistency::addIndexEntry(const FullCellView& val) {
    _tabulateEntry(val, -1);
    _numIndexEntries++;
}

void ColumnIndexConsistency::_updateSuspectList(const FullCellView& cell,
                                                ValidateResults* results) {
    const auto rawHash = _hash(cell);
    const auto hashLower = rawHash % _indexKeyBuckets.size();
    const auto hashUpper = (rawHash / _indexKeyBuckets.size()) % _indexKeyBuckets.size();

    if (_indexKeyBuckets[hashLower].indexKeyCount != 0 ||
        _indexKeyBuckets[hashUpper].indexKeyCount != 0) {
        _suspects.insert(cell.rid);
    }
}

void ColumnIndexConsistency::_tabulateEntry(const FullCellView& cell, int step) {
    const auto rawHash = _hash(cell);
    const auto hashLower = rawHash % _indexKeyBuckets.size();
    const auto hashUpper = (rawHash / _indexKeyBuckets.size()) % _indexKeyBuckets.size();
    auto& lower = _indexKeyBuckets[hashLower];
    auto& upper = _indexKeyBuckets[hashUpper];
    const auto cellSz = cell.path.size() + sizeof(cell.rid) + cell.value.size();

    lower.indexKeyCount += step;
    lower.bucketSizeBytes += cellSz;
    upper.indexKeyCount += step;
    upper.bucketSizeBytes += cellSz;
}
}  // namespace mongo
