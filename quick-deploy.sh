#!/bin/bash

# Quick Deploy Script for Multi-Domain VPS Setup
# This script helps you quickly deploy multiple applications

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

print_status() { echo -e "${GREEN}[INFO]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Show usage
show_help() {
    echo "Quick Deploy Script for Multi-Domain VPS"
    echo ""
    echo "Usage: $0 [COMMAND] [OPTIONS]"
    echo ""
    echo "Commands:"
    echo "  setup-traefik     Set up global Traefik reverse proxy"
    echo "  deploy-retroos    Deploy RetroOS application"
    echo "  deploy-wordpress  Deploy WordPress blog"
    echo "  deploy-api        Deploy API service"
    echo "  list-apps         List all running applications"
    echo "  status            Show status of all services"
    echo "  cleanup           Clean up stopped containers and unused images"
    echo ""
    echo "Options:"
    echo "  -d, --domain      Domain name"
    echo "  -e, --email       Email for Let's Encrypt"
    echo "  -h, --help        Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 setup-traefik -e admin@example.com"
    echo "  $0 deploy-retroos -d retroos.example.com -e admin@example.com"
    echo "  $0 deploy-wordpress -d blog.example.com"
    echo "  $0 status"
}

# Setup Traefik
setup_traefik() {
    local email="$1"
    
    if [[ -z "$email" ]]; then
        print_error "Email is required for Traefik setup"
        exit 1
    fi
    
    print_status "Setting up global Traefik reverse proxy..."
    
    # Create traefik data directory
    mkdir -p traefik-data/logs
    touch traefik-data/acme.json
    chmod 600 traefik-data/acme.json
    
    # Update email in config
    sed -i "s/your-email@example.com/$email/g" traefik-global.yml
    
    # Create network
    docker network create traefik-global 2>/dev/null || true
    
    # Deploy Traefik
    docker-compose -f traefik-global.yml up -d
    
    print_status "✅ Traefik setup complete!"
    echo "   Dashboard: http://$(hostname -I | awk '{print $1}'):8080"
}

# Deploy RetroOS
deploy_retroos() {
    local domain="$1"
    local email="$2"
    
    if [[ -z "$domain" ]]; then
        print_error "Domain is required for RetroOS deployment"
        exit 1
    fi
    
    print_status "Deploying RetroOS to $domain..."
    
    # Use the existing deploy script
    ./deploy-domain.sh -d "$domain" -e "${email:-admin@example.com}"
    
    print_status "✅ RetroOS deployed!"
    echo "   URL: https://$domain"
}

# Deploy WordPress
deploy_wordpress() {
    local domain="$1"
    
    if [[ -z "$domain" ]]; then
        print_error "Domain is required for WordPress deployment"
        exit 1
    fi
    
    print_status "Deploying WordPress to $domain..."
    
    # Create environment file
    cat > wordpress.env << EOF
BLOG_DOMAIN=$domain
MYSQL_PASSWORD=$(openssl rand -base64 32)
MYSQL_ROOT_PASSWORD=$(openssl rand -base64 32)
EOF
    
    # Deploy WordPress
    BLOG_DOMAIN=$domain docker-compose -f examples/wordpress-compose.yml --env-file wordpress.env up -d
    
    print_status "✅ WordPress deployed!"
    echo "   URL: https://$domain"
    echo "   Complete setup by visiting the URL"
}

# Deploy API service
deploy_api() {
    local domain="$1"
    
    if [[ -z "$domain" ]]; then
        print_error "Domain is required for API deployment"
        exit 1
    fi
    
    print_status "Deploying API service to $domain..."
    
    # Create simple API code if it doesn't exist
    if [ ! -d "api-code" ]; then
        mkdir -p api-code
        cat > api-code/package.json << 'EOF'
{
  "name": "simple-api",
  "version": "1.0.0",
  "main": "server.js",
  "scripts": {
    "start": "node server.js"
  },
  "dependencies": {
    "express": "^4.18.0"
  }
}
EOF
        
        cat > api-code/server.js << 'EOF'
const express = require('express');
const app = express();
const PORT = process.env.PORT || 3000;

app.use(express.json());

app.get('/', (req, res) => {
  res.json({ message: 'API is running!', timestamp: new Date().toISOString() });
});

app.get('/health', (req, res) => {
  res.json({ status: 'healthy', uptime: process.uptime() });
});

app.listen(PORT, () => {
  console.log(`API server running on port ${PORT}`);
});
EOF
        
        # Install dependencies
        (cd api-code && npm install)
    fi
    
    # Deploy API
    API_DOMAIN=$domain docker-compose -f examples/api-service-compose.yml up -d
    
    print_status "✅ API service deployed!"
    echo "   URL: https://$domain"
    echo "   Health: https://$domain/health"
}

# List all applications
list_apps() {
    print_status "Running applications:"
    echo ""
    
    # Get all containers with Traefik labels
    docker ps --filter "label=traefik.enable=true" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | head -1
    docker ps --filter "label=traefik.enable=true" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | tail -n +2
    
    echo ""
    print_status "Domains configured:"
    
    # Extract domains from container labels
    docker ps --filter "label=traefik.enable=true" -q | while read container; do
        domain=$(docker inspect $container | jq -r '.[0].Config.Labels | to_entries[] | select(.key | contains("traefik.http.routers") and contains(".rule")) | .value' | grep -o 'Host(`[^`]*`)' | sed 's/Host(`\(.*\)`)/\1/' | head -1)
        name=$(docker inspect $container | jq -r '.[0].Name' | sed 's/^\///')
        if [ ! -z "$domain" ]; then
            echo "   • $domain → $name"
        fi
    done
}

# Show status
show_status() {
    print_status "System Status:"
    echo ""
    
    # Traefik status
    if docker ps | grep -q traefik-global; then
        print_status "✅ Traefik: Running"
    else
        print_warning "❌ Traefik: Not running"
    fi
    
    # Container counts
    running=$(docker ps | wc -l)
    total=$(docker ps -a | wc -l)
    echo "   Containers: $((running-1)) running, $((total-1)) total"
    
    # Disk usage
    echo "   Docker disk usage:"
    docker system df
    
    echo ""
    list_apps
}

# Cleanup
cleanup() {
    print_status "Cleaning up Docker resources..."
    
    # Remove stopped containers
    docker container prune -f
    
    # Remove unused images
    docker image prune -f
    
    # Remove unused volumes
    docker volume prune -f
    
    # Remove unused networks
    docker network prune -f
    
    print_status "✅ Cleanup complete!"
}

# Parse arguments
COMMAND=""
DOMAIN=""
EMAIL=""

while [[ $# -gt 0 ]]; do
    case $1 in
        setup-traefik|deploy-retroos|deploy-wordpress|deploy-api|list-apps|status|cleanup)
            COMMAND="$1"
            shift
            ;;
        -d|--domain)
            DOMAIN="$2"
            shift 2
            ;;
        -e|--email)
            EMAIL="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Execute command
case $COMMAND in
    setup-traefik)
        setup_traefik "$EMAIL"
        ;;
    deploy-retroos)
        deploy_retroos "$DOMAIN" "$EMAIL"
        ;;
    deploy-wordpress)
        deploy_wordpress "$DOMAIN"
        ;;
    deploy-api)
        deploy_api "$DOMAIN"
        ;;
    list-apps)
        list_apps
        ;;
    status)
        show_status
        ;;
    cleanup)
        cleanup
        ;;
    *)
        print_error "No command specified"
        show_help
        exit 1
        ;;
esac