from http.server import HTTPServer, SimpleHTTPRequestHandler
import ssl

class MyHandler(SimpleHTTPRequestHandler):

    def do_GET(self):
        cert = self.connection.getpeercert()
        print("Client certificate:")
        print(cert)
        super().do_GET()

server_address = ('0.0.0.0', 4433)

httpd = HTTPServer(server_address, MyHandler)

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)

context.load_cert_chain(
    certfile="../certificates/server/server_0_88.crt",
    keyfile="../certificates/server/server.key"
)

context.load_verify_locations("../certificates/rootCA/rootCA.crt")

context.verify_mode = ssl.CERT_REQUIRED

httpd.socket = context.wrap_socket(
    httpd.socket,
    server_side=True
)

print("mTLS server running...")

try:
    httpd.serve_forever()
except ssl.SSLError as e:
    print(e) 
