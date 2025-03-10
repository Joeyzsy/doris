// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.rules.implementation;

import org.apache.doris.catalog.ColocateTableIndex;
import org.apache.doris.catalog.Column;
import org.apache.doris.catalog.DistributionInfo;
import org.apache.doris.catalog.Env;
import org.apache.doris.catalog.HashDistributionInfo;
import org.apache.doris.catalog.OlapTable;
import org.apache.doris.catalog.PartitionType;
import org.apache.doris.nereids.properties.DistributionSpec;
import org.apache.doris.nereids.properties.DistributionSpecAny;
import org.apache.doris.nereids.properties.DistributionSpecHash;
import org.apache.doris.nereids.properties.DistributionSpecHash.ShuffleType;
import org.apache.doris.nereids.rules.Rule;
import org.apache.doris.nereids.rules.RuleType;
import org.apache.doris.nereids.trees.expressions.ExprId;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.logical.LogicalOlapScan;
import org.apache.doris.nereids.trees.plans.physical.PhysicalOlapScan;

import com.google.common.collect.Lists;
import com.google.common.collect.Sets;

import java.util.List;
import java.util.Optional;

/**
 * Implementation rule that convert logical OlapScan to physical OlapScan.
 */
public class LogicalOlapScanToPhysicalOlapScan extends OneImplementationRuleFactory {
    @Override
    public Rule build() {
        return logicalOlapScan().then(olapScan ->
                new PhysicalOlapScan(
                        olapScan.getId(),
                        olapScan.getTable(),
                        olapScan.getQualifier(),
                        olapScan.getSelectedIndexId(),
                        olapScan.getSelectedTabletIds(),
                        olapScan.getSelectedPartitionIds(),
                        convertDistribution(olapScan),
                        olapScan.getPreAggStatus(),
                        Optional.empty(),
                        olapScan.getLogicalProperties())
        ).toRule(RuleType.LOGICAL_OLAP_SCAN_TO_PHYSICAL_OLAP_SCAN_RULE);
    }

    private DistributionSpec convertDistribution(LogicalOlapScan olapScan) {
        OlapTable olapTable = olapScan.getTable();
        DistributionInfo distributionInfo = olapTable.getDefaultDistributionInfo();
        ColocateTableIndex colocateTableIndex = Env.getCurrentColocateIndex();
        // When there are multiple partitions, olapScan tasks of different buckets are dispatched in
        // rounded robin algorithm. Therefore, the hashDistributedSpec can be broken except they are in
        // the same stable colocateGroup(CG)
        boolean isBelongStableCG = colocateTableIndex.isColocateTable(olapTable.getId())
                && !colocateTableIndex.isGroupUnstable(colocateTableIndex.getGroup(olapTable.getId()));
        boolean isSelectUnpartition = olapTable.getPartitionInfo().getType() == PartitionType.UNPARTITIONED
                || olapScan.getSelectedPartitionIds().size() == 1;
        if (isBelongStableCG || isSelectUnpartition) {
            if (!(distributionInfo instanceof HashDistributionInfo)) {
                return DistributionSpecAny.INSTANCE;
            }
            HashDistributionInfo hashDistributionInfo = (HashDistributionInfo) distributionInfo;
            List<Slot> output = olapScan.getOutput();
            List<ExprId> hashColumns = Lists.newArrayList();
            List<Column> schemaColumns = olapScan.getTable().getFullSchema();
            for (int i = 0; i < schemaColumns.size(); i++) {
                for (Column column : hashDistributionInfo.getDistributionColumns()) {
                    if (schemaColumns.get(i).equals(column)) {
                        hashColumns.add(output.get(i).getExprId());
                    }
                }
            }
            // TODO: need to consider colocate and dynamic partition and partition number
            return new DistributionSpecHash(hashColumns, ShuffleType.NATURAL, olapScan.getTable().getId(),
                    olapScan.getSelectedIndexId(), Sets.newHashSet(olapScan.getSelectedPartitionIds()));
        } else {
            // RandomDistributionInfo
            return DistributionSpecAny.INSTANCE;
        }
    }
}
