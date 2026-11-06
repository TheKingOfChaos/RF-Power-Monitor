# RF-Power-Monitor update site: Python standard library only, no packages to install
FROM python:3.12-slim

RUN useradd --system --uid 10001 --home-dir /app rfupdate \
    && mkdir -p /data && chown rfupdate /data
WORKDIR /app
COPY rfupdate/ /app/rfupdate/
USER rfupdate
ENV RFUPDATE_DATA=/data PYTHONUNBUFFERED=1
EXPOSE 8071
VOLUME ["/data"]
CMD ["python", "-m", "rfupdate", "--port", "8071"]
