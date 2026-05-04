package webserver;


import container.DIContainer;
import filter.FilterConfig;
import org.h2.tools.Server;
import router.Dispatcher;
import webserver.connection.ConnectionManager;

public class WebServer {

  public static void main(String args[]) throws Exception {
    startH2Console();

    initializeDependencies();
    ServerConfig config = new ServerConfig();
    config.startServer();
  }

  private static void startH2Console() throws Exception {
    Server.createWebServer("-web", "-webAllowOthers", "-tcpAllowOthers").start();
  }

  private static void initializeDependencies() {
    DIContainer.initialize(
        // connection
        ConnectionManager.class,
        //filter
        FilterConfig.class,
        //router
        Dispatcher.class
    );
  }

}
