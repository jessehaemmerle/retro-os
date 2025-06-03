#!/bin/bash

# RetroOS Domain Deployment Script
# Deploy RetroOS with custom domain using global Traefik

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_status() { echo -e "${GREEN}[INFO]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }
print_info() { echo -e "${BLUE}[DEBUG]${NC} $1"; }

# Default values
DOMAIN=""
EMAIL=""
SETUP_TRAEFIK=false
COMPOSE_FILE="docker-compose.domain.yml"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--domain)
            DOMAIN="$2"
            shift 2
            ;;
        -e|--email)
            EMAIL="$2"
            shift 2
            ;;
        --setup-traefik)
            SETUP_TRAEFIK=true
            shift
            ;;
        -h|--help)
            echo "RetroOS Domain Deployment Script"
            echo ""
            echo "Usage: $0 -d DOMAIN -e EMAIL [OPTIONS]"
            echo ""
            echo "Required:"
            echo "  -d, --domain DOMAIN       Your domain name (e.g., retroos.example.com)"
            echo "  -e, --email EMAIL         Your email for Let's Encrypt certificates"
            echo ""
            echo "Options:"
            echo "  --setup-traefik          Also set up global Traefik (first time only)"
            echo "  -h, --help              Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0 -d retroos.example.com -e admin@example.com --setup-traefik"
            echo "  $0 -d retroos.example.com -e admin@example.com"
            exit 0
            ;;
        *)
            print_error "Unknown option $1"
            exit 1
            ;;
    esac
done

# Validate required parameters
if [[ -z "$DOMAIN" ]]; then
    print_error "Domain is required. Use -d or --domain"
    exit 1
fi

if [[ -z "$EMAIL" ]]; then
    print_error "Email is required. Use -e or --email"
    exit 1
fi

echo "🚀 Deploying RetroOS for domain: $DOMAIN"

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    print_error "Docker is not installed. Please install Docker first."
    exit 1
fi

if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
    print_error "Docker Compose is not installed. Please install Docker Compose first."
    exit 1
fi

# Setup Traefik if requested
if [ "$SETUP_TRAEFIK" = true ]; then
    print_status "Setting up global Traefik reverse proxy..."
    
    # Create traefik data directory
    mkdir -p traefik-data/logs
    touch traefik-data/acme.json
    chmod 600 traefik-data/acme.json
    
    # Update Traefik configuration with user's email
    sed -i "s/your-email@example.com/$EMAIL/g" traefik-global.yml
    
    # Create the global network
    docker network create traefik-global 2>/dev/null || true
    
    # Deploy Traefik
    print_status "Starting Traefik..."
    docker-compose -f traefik-global.yml up -d
    
    print_status "Waiting for Traefik to start..."
    sleep 10
    
    # Check if Traefik is running
    if docker ps | grep -q traefik-global; then
        print_status "✅ Traefik is running successfully"
    else
        print_error "❌ Traefik failed to start"
        docker-compose -f traefik-global.yml logs traefik
        exit 1
    fi
else
    # Ensure the Traefik network exists
    if ! docker network ls | grep -q traefik-global; then
        print_error "Traefik network not found. Run with --setup-traefik first, or create the network manually."
        exit 1
    fi
    
    # Check if Traefik is running
    if ! docker ps | grep -q traefik-global; then
        print_warning "Traefik doesn't seem to be running. You may need to start it first."
    fi
fi

# Create environment file
print_status "Creating environment configuration..."
cat > .env << EOF
DOMAIN=$DOMAIN
TRAEFIK_EMAIL=$EMAIL
MONGO_URL=mongodb://mongodb:27017
DB_NAME=retroos
REACT_APP_BACKEND_URL=https://$DOMAIN
COMPOSE_PROJECT_NAME=retroos
EOF

# Update backend environment
print_status "Configuring backend environment..."
cat > backend/.env << EOF
MONGO_URL=mongodb://mongodb:27017
DB_NAME=retroos
EOF

# Update frontend environment
print_status "Configuring frontend environment..."
cat > frontend/.env << EOF
REACT_APP_BACKEND_URL=https://$DOMAIN
EOF

# Stop any existing RetroOS containers
print_status "Stopping any existing RetroOS containers..."
docker-compose -f $COMPOSE_FILE down 2>/dev/null || true

# Build and start services
print_status "Building and starting RetroOS services..."
docker-compose -f $COMPOSE_FILE up --build -d

# Wait for services to be ready
print_status "Waiting for services to start..."
sleep 15

# Check service status
print_status "Checking service status..."
docker-compose -f $COMPOSE_FILE ps

echo ""
print_status "🎉 Deployment completed!"
echo ""
echo "📱 Your RetroOS is available at:"
echo "   • Main Application: https://$DOMAIN"
echo "   • API Documentation: https://$DOMAIN/api/docs"
echo ""
echo "🔧 Management Commands:"
echo "   • View logs:        docker-compose -f $COMPOSE_FILE logs -f"
echo "   • Stop services:    docker-compose -f $COMPOSE_FILE down"
echo "   • Restart:          docker-compose -f $COMPOSE_FILE restart"
echo "   • Update:           docker-compose -f $COMPOSE_FILE up --build -d"
echo ""
echo "🌐 Global Traefik:"
echo "   • Dashboard:        http://$(hostname -I | awk '{print $1}'):8080"
echo "   • SSL Certificates: Automatically managed by Let's Encrypt"
echo ""

if [ "$SETUP_TRAEFIK" = true ]; then
    echo "📋 Next Steps for Additional Apps:"
    echo "   1. Create a new docker-compose file for your next app"
    echo "   2. Add Traefik labels with your new domain"
    echo "   3. Connect to 'traefik-global' network"
    echo "   4. Deploy with: docker-compose up -d"
    echo ""
fi

print_status "Setup complete! Your RetroOS should be accessible at https://$DOMAIN in a few minutes."
print_warning "Note: SSL certificate generation may take a few minutes on first deployment."