#include <cstdlib>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "common/config.h"
#include "common/logger.h"
#include "service/monitor_service.h"
#include "service/query_service.h"
#include "storage/mysql_storage.h"
#include "alert/alert_engine.h"
#include "scheduler/weighted_scheduler.h"

int main(int argc, char** argv) {
  // 支持两种写法：-c <path> 或直接 <path>
  std::string conf_path = "configs/manager.conf";
  if (argc > 1) {
    if (std::string(argv[1]) == "-c" && argc > 2)
      conf_path = argv[2];
    else if (std::string(argv[1]) != "-c")
      conf_path = argv[1];
  }

  common::Config cfg;
  if (!cfg.load(conf_path)) return 1;

  manager::MysqlStorage::Options mopt;
  mopt.enable = cfg.get_bool("mysql", "enable", false);
  if (const char* env_host = getenv("MYSQL_HOST")) {
    mopt.host = env_host;
  } else {
    mopt.host = cfg.get("mysql", "host", "127.0.0.1");
  }
  mopt.port = cfg.get_int("mysql", "port", 3306);
  mopt.user = cfg.get("mysql", "user", "root");
  mopt.password = cfg.get("mysql", "password", "");
  mopt.db = cfg.get("mysql", "db", "monitor_db");

  manager::AlertEngine::Options aopt;
  aopt.cpu_max = cfg.get_double("alert", "cpu_max", 90.0);
  aopt.mem_max = cfg.get_double("alert", "mem_max", 90.0);
  aopt.cpu_jump = cfg.get_double("alert", "cpu_jump", 30.0);

  manager::WeightedScheduler::Weights w;
  w.cpu = cfg.get_double("scheduler", "w_cpu", 0.5);
  w.mem = cfg.get_double("scheduler", "w_mem", 0.3);
  w.net = cfg.get_double("scheduler", "w_net", 0.1);
  w.disk = cfg.get_double("scheduler", "w_disk", 0.1);

  manager::MysqlStorage storage(mopt);
  if (mopt.enable && !storage.connect()) {
    LOG_WARN("mysql unavailable, metrics will not be persisted");
  }
  manager::AlertEngine alert(aopt);
  manager::WeightedScheduler scheduler(w);
  manager::MonitorService service(&storage, &alert, &scheduler);
  manager::QueryService query_service(&storage);

  const std::string addr = cfg.get("manager", "listen_addr", "0.0.0.0:50051");
  grpc::ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.RegisterService(&query_service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    LOG_ERROR("manager: listen on %s failed", addr.c_str());
    return 1;
  }
  LOG_INFO("manager listening on %s (mysql %s)", addr.c_str(),
           mopt.enable ? "on" : "off");

  server->Wait();
  return 0;
}
