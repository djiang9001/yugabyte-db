// Copyright (c) YugaByte, Inc.
package org.yb.loadtester;

import com.google.common.net.HostAndPort;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.yugabyte.sample.Main;
import com.yugabyte.sample.common.CmdLineOpts;
import org.junit.After;
import org.junit.Before;
import org.junit.Ignore;
import org.junit.Test;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.Common;
import org.yb.client.ChangeConfigResponse;
import org.yb.minicluster.MiniYBCluster;
import org.yb.client.ModifyMasterClusterConfigBlacklist;
import org.yb.client.TestUtils;
import org.yb.client.YBClient;
import org.yb.cql.BaseCQLTest;
import org.yb.minicluster.MiniYBDaemon;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.URL;
import java.net.URLConnection;
import java.util.*;

import static junit.framework.TestCase.assertFalse;
import static junit.framework.TestCase.assertTrue;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

/**
 * This is an integration test that ensures we can expand, shrink and fully move a YB cluster
 * without any significant impact to a running load test.
 */
public class TestClusterEdits extends BaseCQLTest {

  private static final Logger LOG = LoggerFactory.getLogger(TestClusterEdits.class);

  private LoadTester loadTesterRunnable = null;

  private Thread loadTesterThread = null;

  private String cqlContactPoints = null;

  private YBClient client = null;

  private static final String WORKLOAD = "CassandraStockTicker";

  // Number of ops to wait for between significant events in the test.
  private static final int NUM_OPS_INCREMENT = 10000;

  // Timeout to wait for desired number of ops.
  private static final int WAIT_FOR_OPS_TIMEOUT_MS = 60000; // 60 seconds

  // Timeout to wait for load balancing to complete.
  private static final int LOADBALANCE_TIMEOUT_MS = 60000; // 60 seconds

  // Timeout to wait for cluster move to complete.
  private static final int CLUSTER_MOVE_TIMEOUT_MS = 300000; // 5 mins

  // Timeout for test completion.
  private static final int TEST_TIMEOUT_SEC = 600; // 10 mins

  // Timeout to wait for a new master to come up.
  private static final int NEW_MASTER_TIMEOUT_MS = 10000; // 10 seconds.

  // The total number of ops seen so far.
  private static long totalOps = 0;

  @Override
  public int getTestMethodTimeoutSec() {
    return TEST_TIMEOUT_SEC;
  }

  @Override
  protected void afterStartingMiniCluster() {
    cqlContactPoints = miniCluster.getCQLContactPointsAsString();
    client = miniCluster.getClient();
    LOG.info("Retrieved contact points from cluster: " + cqlContactPoints);
  }

  @Before
  public void startLoadTester() throws Exception {
    // Start the load tester.
    LOG.info("Using contact points for load tester: " + cqlContactPoints);
    loadTesterRunnable = new LoadTester(WORKLOAD, cqlContactPoints);
    loadTesterThread = new Thread(loadTesterRunnable);
    loadTesterThread.start();
    LOG.info("Loadtester start.");
  }

  @After
  public void stopLoadTester() throws Exception {
    // Stop load tester and exit.
    if (loadTesterRunnable != null) {
      loadTesterRunnable.stopLoadTester();
      loadTesterRunnable = null;
    }

    if (loadTesterThread != null) {
      loadTesterThread.join();
      loadTesterThread = null;
    }

    LOG.info("Loadtester stopped.");
  }

  @Override
  protected void afterBaseCQLTestTearDown() throws Exception {
    // We need to destroy the mini cluster after BaseCQLTest cleans up all the tables and keyspaces.
    destroyMiniCluster();
    LOG.info("Stopped minicluster.");
  }

  private class LoadTester implements Runnable {

    private final Main testRunner;

    private volatile boolean testRunnerFailed = false;

    public LoadTester(String workload, String cqlContactPoints) throws Exception {
      String args[] = {"--workload", workload, "--nodes", cqlContactPoints};
      CmdLineOpts configuration = CmdLineOpts.createFromArgs(args);
      testRunner = new Main(configuration);
    }

    public void stopLoadTester() {
      testRunner.stopAllThreads();
    }

    @Override
    public void run() {
      try {
        testRunner.run();
      } catch (Exception e) {
        LOG.error("Load tester exited!", e);
        testRunnerFailed = true;
      }
    }

    public boolean hasFailures() {
      return testRunner.hasFailures() || testRunnerFailed;
    }

    public boolean hasThreadFailed() {
      return testRunner.hasThreadFailed();
    }

    public int getNumExceptions() {
      return testRunner.getNumExceptions();
    }

    public void waitNumOpsAtLeast(long expectedNumOps) throws Exception {
      TestUtils.waitFor(() -> testRunner.numOps() >= expectedNumOps, WAIT_FOR_OPS_TIMEOUT_MS);
      totalOps = testRunner.numOps();
      LOG.info("Num Ops: " + totalOps + ", Expected: " + expectedNumOps);
      assertTrue(totalOps >= expectedNumOps);
    }
  }

  private JsonElement getMetricsJson(String host, int port) throws Exception {
    // Retrieve metrics json.
    URL metrics = new URL(String.format("http://%s:%d/metrics", host, port));
    URLConnection yc = metrics.openConnection();

    BufferedReader in = new BufferedReader(new InputStreamReader(yc.getInputStream()));
    String json = "";
    try {
      String inputLine;
      while ((inputLine = in.readLine()) != null) {
        json += inputLine;
      }
    } finally {
      in.close();
    }
    JsonParser parser = new JsonParser();
    return parser.parse(json);
  }

  private void verifyMetrics(int minOps) throws Exception {
    for (MiniYBDaemon ts : miniCluster.getTabletServers().values()) {

      // This is what the json looks likes:
      //[
      //  {
      //    "type": "server",
      //    "id": "yb.cqlserver",
      //    "attributes": {},
      //    "metrics": [
      //      {
      //        "name": "handler_latency_yb_cqlserver_SQLProcessor_SelectStmt",
      //        "total_count": 0,
      //        ...
      //      },
      //      {
      //        "name": "handler_latency_yb_cqlserver_SQLProcessor_InsertStmt",
      //        "total_count": 0,
      //        ...
      //      }
      //    ]
      //  }
      //]
      // Now parse the json.
      JsonElement jsonTree = getMetricsJson(ts.getLocalhostIP(), ts.getCqlWebPort());
      JsonObject jsonObject = jsonTree.getAsJsonArray().get(0).getAsJsonObject();
      int numOps = 0;
      for (JsonElement jsonElement : jsonObject.getAsJsonArray("metrics")) {
        JsonObject jsonMetric = jsonElement.getAsJsonObject();
        String metric_name = jsonMetric.get("name").getAsString();
        if (metric_name.equals("handler_latency_yb_cqlserver_SQLProcessor_ExecuteRequest")) {
          numOps += jsonMetric.get("total_count").getAsInt();
        }
      }
      LOG.info("TSERVER: " + ts.getLocalhostIP() + ", num_ops: " + numOps + ", min_ops: " + minOps);
      assertTrue(numOps >= minOps);
    }
  }

  private void verifyExpectedLiveTServers(int expected_live) throws Exception {
    // Wait for metrics to be submitted.
    Thread.sleep(2 * MiniYBCluster.CATALOG_MANAGER_BG_TASK_WAIT_MS);

    // Now verify leader master has expected number of live tservers.
    HostAndPort masterHostAndPort = client.getLeaderMasterHostAndPort();
    int masterLeaderWebPort =
      miniCluster.getMasters().get(masterHostAndPort).getWebPort();
    JsonElement jsonTree = getMetricsJson(masterHostAndPort.getHostText(), masterLeaderWebPort);
    for (JsonElement element : jsonTree.getAsJsonArray()) {
      JsonObject object = element.getAsJsonObject();
      if (object.get("type").getAsString().equals("cluster")) {
        for (JsonElement metric : object.getAsJsonArray("metrics")) {
          JsonObject metric_object = metric.getAsJsonObject();
          if (metric_object.get("name").getAsString().equals("num_tablet_servers_live")) {
            assertEquals(expected_live, metric_object.get("value").getAsInt());
            LOG.info("Found live tservers: " + expected_live);
            return;
          }
        }
      }
    }
    fail("Didn't find live tserver metric");
  }

  private void performFullMasterMove() throws Exception {
    // Create a copy to store original list.
    Map<HostAndPort, MiniYBDaemon> originalMasters = new HashMap<>(miniCluster.getMasters());
    for (HostAndPort originalMaster : originalMasters.keySet()) {
      // Add new master.
      HostAndPort masterRpcHostPort = miniCluster.startShellMaster();

      // Wait for new master to be online.
      assertTrue(client.waitForMaster(masterRpcHostPort, NEW_MASTER_TIMEOUT_MS));

      LOG.info("New master online: " + masterRpcHostPort.toString());

      // Add new master to the config.
      ChangeConfigResponse response = client.changeMasterConfig(masterRpcHostPort.getHostText(),
        masterRpcHostPort.getPort(), true);
      assertFalse("ChangeConfig has error: " + response.errorMessage(), response.hasError());

      LOG.info("Added new master to config: " + masterRpcHostPort.toString());

      // Remove old master.
      response = client.changeMasterConfig(originalMaster.getHostText(), originalMaster.getPort(),
        false);
      assertFalse("ChangeConfig has error: " + response.errorMessage(), response.hasError());

      LOG.info("Removed old master from config: " + originalMaster.toString());

      // Kill the old master.
      miniCluster.killMasterOnHostPort(originalMaster);

      LOG.info("Killed old master: " + originalMaster.toString());

      // Wait for hearbeat interval to ensure tservers pick up the new masters.
      Thread.sleep(2 * MiniYBCluster.TSERVER_HEARTBEAT_INTERVAL_MS);

      LOG.info("Done waiting for new leader");

      // Verify no load tester errors.
      assertFalse(loadTesterRunnable.hasFailures());
    }
  }

  private void addNewTServers() throws Exception {
    // Now double the number of tservers to expand the cluster and verify load spreads.
    for (int i = 0; i < NUM_TABLET_SERVERS; i++) {
      miniCluster.startTServer(null);
    }

    // Wait for the CQL client to discover the new nodes.
    Thread.sleep(2 * MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 1000);
  }

  private void verifyStateAfterTServerAddition() throws Exception {
    // Wait for some ops across the entire cluster.
    loadTesterRunnable.waitNumOpsAtLeast(totalOps + NUM_OPS_INCREMENT);

    // Verify no failures in load tester.
    assertFalse(loadTesterRunnable.hasFailures());

    // Verify metrics for all tservers.
    verifyMetrics(0);

    // Verify live tservers.
    verifyExpectedLiveTServers(2 * NUM_TABLET_SERVERS);
  }

  private void removeTServers(Map<HostAndPort, MiniYBDaemon> originalTServers) throws Exception {
    // Retrieve existing config, set blacklist and reconfigure cluster.
    List<Common.HostPortPB> blacklisted_hosts = new ArrayList<>();
    for (Map.Entry<HostAndPort, MiniYBDaemon> ts : originalTServers.entrySet()) {
      Common.HostPortPB hostPortPB = Common.HostPortPB.newBuilder()
        .setHost(ts.getKey().getHostText())
        .setPort(ts.getKey().getPort())
        .build();
      blacklisted_hosts.add(hostPortPB);
    }

    ModifyMasterClusterConfigBlacklist operation =
      new ModifyMasterClusterConfigBlacklist(client, blacklisted_hosts, true);
    try {
      operation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      fail(e.getMessage());
    }

    // Wait for the move to complete.
    TestUtils.waitFor(() -> {
      verifyExpectedLiveTServers(2 * NUM_TABLET_SERVERS);
      final double move_completion = client.getLoadMoveCompletion().getPercentCompleted();
      LOG.info("Move completion percent: " + move_completion);
      return move_completion >= 100;
    }, CLUSTER_MOVE_TIMEOUT_MS);

    assertEquals(100, (int) client.getLoadMoveCompletion().getPercentCompleted());

    // Kill the old tablet servers.
    // TODO: Check that the blacklisted tablet servers have no tablets assigned.
    for (HostAndPort rpcPort : originalTServers.keySet()) {
      miniCluster.killTabletServerOnHostPort(rpcPort);
    }

    // Wait for heartbeats to expire.
    Thread.sleep(MiniYBCluster.TSERVER_HEARTBEAT_TIMEOUT_MS * 2);

    // Verify live tservers.
    verifyExpectedLiveTServers(NUM_TABLET_SERVERS);

  }

  private void verifyClusterHealth() throws Exception {
    // Wait for some ops.
    loadTesterRunnable.waitNumOpsAtLeast(totalOps + NUM_OPS_INCREMENT);

    // Wait for some more ops and verify no exceptions.
    loadTesterRunnable.waitNumOpsAtLeast(totalOps + NUM_OPS_INCREMENT);
    assertEquals(0, loadTesterRunnable.getNumExceptions());

    // Verify metrics for all tservers.
    verifyMetrics(NUM_OPS_INCREMENT / NUM_TABLET_SERVERS);

    // Wait for heartbeats to expire.
    Thread.sleep(MiniYBCluster.TSERVER_HEARTBEAT_TIMEOUT_MS * 2);

    // Verify live tservers.
    verifyExpectedLiveTServers(NUM_TABLET_SERVERS);

    // Verify no failures in the load tester.
    assertFalse(loadTesterRunnable.hasFailures());
  }

  private void performTServerExpandShrink(boolean fullMove) throws Exception {
    // Create a copy to store original tserver list.
    Map<HostAndPort, MiniYBDaemon> originalTServers = new HashMap<>(miniCluster.getTabletServers());
    assertEquals(NUM_TABLET_SERVERS, originalTServers.size());

    addNewTServers();

    // In the full move case, we don't wait for load balancing.
    if (!fullMove) {
      // Wait for the load to be balanced across the cluster.
      assertTrue(client.waitForLoadBalance(LOADBALANCE_TIMEOUT_MS, NUM_TABLET_SERVERS * 2));
    }

    verifyStateAfterTServerAddition();

    LOG.info("Cluster Expand Done!");

    removeTServers(originalTServers);

    LOG.info("Cluster Shrink Done!");
  }

  @Test(timeout = TEST_TIMEOUT_SEC * 1000) // 10 minutes.
  public void testClusterFullMove() throws Exception {
    // Wait for load tester to generate traffic.
    loadTesterRunnable.waitNumOpsAtLeast(totalOps + NUM_OPS_INCREMENT);

    // First try to move all the masters.
    performFullMasterMove();

    // Wait for some ops and verify no failures in load tester.
    loadTesterRunnable.waitNumOpsAtLeast(totalOps + NUM_OPS_INCREMENT);
    assertFalse(loadTesterRunnable.hasFailures());

    // Now perform a full tserver move.
    performTServerExpandShrink(true);

    verifyClusterHealth();
  }

  @Test(timeout = TEST_TIMEOUT_SEC * 1000) // 10 minutes.
  public void testClusterExpandAndShrink() throws Exception {
    // Wait for load tester to generate traffic.
    loadTesterRunnable.waitNumOpsAtLeast(totalOps + NUM_OPS_INCREMENT);

    // Now perform a tserver expand and shrink.
    performTServerExpandShrink(false);

    verifyClusterHealth();
  }
}
