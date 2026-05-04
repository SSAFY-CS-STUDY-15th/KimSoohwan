package router;

import annotation.Singleton;
import http.request.HttpRequest;
import http.response.HttpResponse;

import http.response.HttpResponseFactory;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import router.servlet.AppServlet;
import router.servlet.StaticServlet;

import java.util.Optional;

@Singleton
public class Dispatcher {
  private final Logger logger = LoggerFactory.getLogger(Dispatcher.class);
  private final StaticServlet defaultServlet;
  private final AppServlet appServlet;

  public Dispatcher(StaticServlet staticServlet, AppServlet appServlet) {
    this.defaultServlet = staticServlet;
    this.appServlet = appServlet;
  }

  public HttpResponse dispatch(HttpRequest request) {
    try {
      Optional<HttpResponse> response = appServlet.service(request);
      if (response.isPresent()) {
        return response.get();
      }
      response = defaultServlet.service(request);
      return response.orElseGet(HttpResponseFactory::notFound);
    } catch (Exception e) {
      logger.error(e.getMessage());
      return HttpResponseFactory.internalServerError(e.getMessage());
    }

  }


}
